#include "query.hpp"
#include "categories_holder.hpp"
#include "latlon_match.hpp"
#include "search_trie_matching.hpp"

#include "../indexer/feature_visibility.hpp"
#include "../indexer/scales.hpp"
#include "../indexer/search_delimiters.hpp"
#include "../indexer/string_search_utils.hpp"

#include "../base/exception.hpp"
#include "../base/stl_add.hpp"
#include "../base/logging.hpp"

#include "../std/algorithm.hpp"
#include "../std/scoped_ptr.hpp"


namespace search
{
namespace impl
{
namespace
{

uint32_t KeywordMatch(strings::UniChar const * sA, uint32_t sizeA,
                      strings::UniChar const * sB, uint32_t sizeB,
                      uint32_t maxCost)
{
  if (sizeA != sizeB)
    return maxCost + 1;
  strings::UniChar const * const endA = sA + sizeA;
  while (sA != endA)
    if (*sA++ != *sB++)
      return maxCost + 1;
  return 0;
}

uint32_t PrefixMatch(strings::UniChar const * sA, uint32_t sizeA,
                     strings::UniChar const * sB, uint32_t sizeB,
                     uint32_t maxCost)
{
  if (sizeA > sizeB)
    return maxCost + 1;
  strings::UniChar const * const endA = sA + sizeA;
  while (sA != endA)
    if (*sA++ != *sB++)
      return maxCost + 1;
  return 0;
}

inline uint32_t GetMaxKeywordMatchScore() { return 512; }
inline uint32_t GetMaxPrefixMatchScore(int size)
{
  if (size < 3)
    return 1;
  if (size < 6)
    return 256;
  return 512;
}

template <typename UniStringPtrVectorT>
inline KeywordMatcher MakeMatcher(UniStringPtrVectorT const & tokens,
                                  strings::UniString const & prefix)
{
  return KeywordMatcher(tokens.empty() ? NULL : &tokens[0], tokens.size(),
                        prefix,
                        GetMaxKeywordMatchScore(), GetMaxPrefixMatchScore(prefix.size()),
                        &KeywordMatch, &PrefixMatch);
}

struct FeatureProcessor
{
  DECLARE_EXCEPTION(StopException, RootException);

  Query & m_query;

  explicit FeatureProcessor(Query & query) : m_query(query) {}

  void operator () (FeatureType const & feature) const
  {
    if (m_query.GetTerminateFlag())
    {
      LOG(LDEBUG, ("Found terminate search flag", m_query.GetQueryText(), m_query.GetViewport()));
      MYTHROW(StopException, ());
    }

    uint32_t keywordsSkipMask = 0;
    FeatureType::GetTypesFn types;
    feature.ForEachTypeRef(types);
    for (size_t i = 0; i < types.m_size; ++i)
      keywordsSkipMask |= m_query.GetKeywordsToSkipForType(types.m_types[i]);

    vector<strings::UniString> const & queryKeywords = m_query.GetKeywords();
    ASSERT_LESS(queryKeywords.size(), 32, ());
    buffer_vector<strings::UniString const *, 32> keywords;
    keywords.reserve(queryKeywords.size());
    for (size_t i = 0; i < queryKeywords.size() && i < 32; ++i)
      if (!(keywordsSkipMask & (1 << i)))
        keywords.push_back(&queryKeywords[i]);

    KeywordMatcher matcher(MakeMatcher(keywords, m_query.GetPrefix()));
    feature.ForEachNameRef(matcher);
    if (matcher.GetPrefixMatchScore() <= GetMaxPrefixMatchScore(m_query.GetPrefix().size()))
    {
      uint32_t const matchScore = matcher.GetMatchScore();
      if (matchScore <= GetMaxKeywordMatchScore())
      {
        pair<int, int> const scaleRange = feature::DrawableScaleRangeForText(feature);
        if (scaleRange.first < 0)
          return;

        m_query.AddResult(IntermediateResult(m_query.GetViewport(),
                                             feature,
                                             matcher.GetBestMatchName(),
                                             matchScore,
                                             scaleRange.first));
      }
    }
  }
};

}  // unnamed namespace

Query::Query(string const & query, m2::RectD const & viewport, IndexType const * pIndex,
             Engine * pEngine, CategoriesHolder * pCategories,
             TrieIterator * pTrieRoot, FeaturesVector * pFeatures)
  : m_queryText(query), m_queryUniText(NormalizeAndSimplifyString(query)),
    m_viewport(viewport),
    m_pCategories(pCategories),
    m_pTrieRoot(pTrieRoot),
    m_pFeatures(pFeatures),
    m_pIndex(pIndex ? new IndexType(*pIndex) : NULL),
    m_resultsRemaining(10),
    m_pEngine(pEngine), m_bTerminate(false)
{
  search::Delimiters delims;
  SplitUniString(m_queryUniText, MakeBackInsertFunctor(m_keywords), delims);
  if (!m_keywords.empty() && !delims(strings::LastUniChar(query)))
  {
    m_prefix.swap(m_keywords.back());
    m_keywords.pop_back();
  }

  ASSERT_LESS_OR_EQUAL(m_keywords.size(), 31, ());
  if (m_keywords.size() > 31)
    m_keywords.resize(31);
}

Query::~Query()
{
  if (m_pEngine)
    m_pEngine->OnQueryDelete(this);
}

void Query::Search(function<void (Result const &)> const & f)
{
  if (m_bTerminate)
    return;

  // Lat lon matching.
  {
    double lat, lon, latPrec, lonPrec;
    if (search::MatchLatLon(m_queryText, lat, lon, latPrec, lonPrec))
    {
      double const precision = 5.0 * max(0.0001, min(latPrec, lonPrec));  // Min 55 meters
      AddResult(IntermediateResult(m_viewport, lat, lon, precision));
    }
  }

  if (m_bTerminate)
    return;

  // Category matching.
  if (m_pCategories)
  {
    for (CategoriesHolder::const_iterator iCategory = m_pCategories->begin();
         iCategory != m_pCategories->end(); ++iCategory)
    {
      string bestPrefixMatch;
      // TODO: Use 1 here for exact match?
      static int const PREFIX_LEN_BITS = 5;
      int bestPrefixMatchPenalty =
          ((GetMaxPrefixMatchScore(m_prefix.size()) + 1) << PREFIX_LEN_BITS) - 1;

      for (vector<Category::Name>::const_iterator iName = iCategory->m_synonyms.begin();
           iName != iCategory->m_synonyms.end(); ++iName)
      {
        if (!m_keywords.empty())
        {
          // TODO: Insert spelling here?
          vector<strings::UniString> tokens;
          SplitUniString(NormalizeAndSimplifyString(iName->m_name),
                         MakeBackInsertFunctor(tokens),
                         Delimiters());
          size_t const n = tokens.size();
          if (m_keywords.size() >= n)
          {
            if (equal(tokens.begin(), tokens.end(), m_keywords.begin()))
              for (vector<uint32_t>::const_iterator iType = iCategory->m_types.begin();
                   iType != iCategory->m_types.end(); ++iType)
                m_keywordsToSkipForType[*iType] |= (1 << n) - 1;
            if (equal(tokens.begin(), tokens.end(), m_keywords.end() - n))
              for (vector<uint32_t>::const_iterator iType = iCategory->m_types.begin();
                   iType != iCategory->m_types.end(); ++iType)
                m_keywordsToSkipForType[*iType] |= ((1 << n) - 1) << (m_keywords.size() - n);
          }
        }
        else if (!m_prefix.empty())
        {
          // TODO: Prefer user languages here.
          if (m_prefix.size() >= iName->m_prefixLengthToSuggest)
          {
            KeywordMatcher matcher = MakeMatcher(vector<strings::UniString const *>(), m_prefix);
            matcher.ProcessNameToken(string(), NormalizeAndSimplifyString(iName->m_name));
            ASSERT_LESS(iName->m_prefixLengthToSuggest, 1 << PREFIX_LEN_BITS, ());
            int const penalty =
                (matcher.GetPrefixMatchScore() << PREFIX_LEN_BITS) + iName->m_prefixLengthToSuggest;
            if (penalty < bestPrefixMatchPenalty)
            {
              bestPrefixMatchPenalty = penalty;
              bestPrefixMatch = iName->m_name;
            }
          }
        }
      }

      if (!bestPrefixMatch.empty())
      {
        AddResult(IntermediateResult(bestPrefixMatch, bestPrefixMatch + ' ',
                                     bestPrefixMatchPenalty));
      }
    }
  }

  if (m_bTerminate)
    return;

  int const scale = scales::GetScaleLevel(m_viewport);

  if (scale > scales::GetUpperWorldScale())
  {
    // First - make features matching for viewport in current country.
    try
    {
      FeatureProcessor featureProcessor(*this);
      /// @todo Tune depth scale search (1 is no enough)
      m_pIndex->ForEachInRect(featureProcessor, m_viewport, min(scales::GetUpperScale(), scale + 7));
    }
    catch (FeatureProcessor::StopException &)
    {
      LOG(LDEBUG, ("FeatureProcessor::StopException"));
    }
  }

  if (m_bTerminate)
    return;

  FlushResults(f);
  if (m_resultsRemaining == 0)
  {
    f(Result(string(), string()));  // Send last search result marker.
    return;
  }

  if (m_bTerminate)
    return;

  if (m_pTrieRoot)
  {
    // Make features matching in world trie.
    search::MatchAgainstTrie(*this, *m_pTrieRoot, *m_pFeatures);
  }

  if (m_bTerminate)
    return;

  FlushResults(f);
  f(Result(string(), string()));  // Send last search result marker.
}

void Query::FlushResults(const function<void (const Result &)> &f)
{
  vector<Result> results;
  results.reserve(m_results.size());
  while (!m_results.empty())
  {
    results.push_back(m_results.top().GenerateFinalResult());
    m_results.pop();
  }
  for (vector<Result>::const_reverse_iterator it = results.rbegin(); it != results.rend(); ++it)
    f(*it);
  m_resultsRemaining = max(0, m_resultsRemaining - static_cast<int>(results.size()));
}

void Query::SearchAndDestroy(function<void (const Result &)> const & f)
{
  scoped_ptr<Query> scopedThis(this);
  UNUSED_VALUE(scopedThis);
  Search(f);
}

void Query::AddResult(IntermediateResult const & result)
{
  if (m_results.size() < m_resultsRemaining)
    m_results.push(result);
  else if (result < m_results.top())
  {
    m_results.pop();
    m_results.push(result);
  }
}

uint32_t Query::GetKeywordsToSkipForType(uint32_t const type) const
{
  unordered_map<uint32_t, uint32_t>::const_iterator it = m_keywordsToSkipForType.find(type);
  if (it == m_keywordsToSkipForType.end())
    return 0;
  return it->second;
}

}  // namespace search::impl
}  // namespace search
