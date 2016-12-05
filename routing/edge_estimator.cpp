#include "routing/edge_estimator.hpp"

#include "traffic/traffic_info.hpp"

#include "std/algorithm.hpp"

using namespace traffic;

namespace
{
double CalcTrafficFactor(SpeedGroup speedGroup)
{
  double const percentage =
      0.01 * static_cast<double>(kSpeedGroupThresholdPercentage[static_cast<size_t>(speedGroup)]);
  CHECK_GREATER(percentage, 0.0, ("speedGroup:", speedGroup));
  return 1.0 / percentage;
}
}  // namespace

namespace routing
{
double constexpr kKMPH2MPS = 1000.0 / (60 * 60);

inline double TimeBetweenSec(m2::PointD const & from, m2::PointD const & to, double speedMPS)
{
  ASSERT_GREATER(speedMPS, 0.0, ());

  double const distanceM = MercatorBounds::DistanceOnEarth(from, to);
  return distanceM / speedMPS;
}

class CarEdgeEstimator : public EdgeEstimator
{
public:
  CarEdgeEstimator(IVehicleModel const & vehicleModel, traffic::TrafficCache const & getter);

  // EdgeEstimator overrides:
  void Start(MwmSet::MwmId const & mwmId) override;
  void Finish() override;
  double CalcEdgesWeight(uint32_t featureId, RoadGeometry const & road, uint32_t pointFrom,
                         uint32_t pointTo) const override;
  double CalcHeuristic(m2::PointD const & from, m2::PointD const & to) const override;

private:
  TrafficCache const & m_trafficGetter;
  shared_ptr<traffic::TrafficInfo> m_trafficInfo;
  double const m_maxSpeedMPS;
};

CarEdgeEstimator::CarEdgeEstimator(IVehicleModel const & vehicleModel,
                                   traffic::TrafficCache const & getter)
  : m_trafficGetter(getter), m_maxSpeedMPS(vehicleModel.GetMaxSpeed() * kKMPH2MPS)
{
}

void CarEdgeEstimator::Start(MwmSet::MwmId const & mwmId)
{
  m_trafficInfo = m_trafficGetter.GetTrafficInfo(mwmId);
}

void CarEdgeEstimator::Finish()
{
  m_trafficInfo.reset();
}

double CarEdgeEstimator::CalcEdgesWeight(uint32_t featureId, RoadGeometry const & road,
                                         uint32_t pointFrom, uint32_t pointTo) const
{
  uint32_t const start = min(pointFrom, pointTo);
  uint32_t const finish = max(pointFrom, pointTo);
  ASSERT_LESS(finish, road.GetPointsCount(), ());

  double result = 0.0;
  double const speedMPS = road.GetSpeed() * kKMPH2MPS;
  auto const dir = pointFrom < pointTo ? TrafficInfo::RoadSegmentId::kForwardDirection
                                       : TrafficInfo::RoadSegmentId::kReverseDirection;
  for (uint32_t i = start; i < finish; ++i)
  {
    double edgeWeight = TimeBetweenSec(road.GetPoint(i), road.GetPoint(i + 1), speedMPS);
    if (m_trafficInfo)
    {
      SpeedGroup const speedGroup =
          m_trafficInfo->GetSpeedGroup(TrafficInfo::RoadSegmentId(featureId, i, dir));
      ASSERT_LESS(speedGroup, SpeedGroup::Count, ());
      edgeWeight *= CalcTrafficFactor(speedGroup);
    }
    result += edgeWeight;
  }

  return result;
}

double CarEdgeEstimator::CalcHeuristic(m2::PointD const & from, m2::PointD const & to) const
{
  return TimeBetweenSec(from, to, m_maxSpeedMPS);
}
}  // namespace

namespace routing
{
// static
shared_ptr<EdgeEstimator> EdgeEstimator::CreateForCar(IVehicleModel const & vehicleModel,
                                                      traffic::TrafficCache const & getter)
{
  return make_shared<CarEdgeEstimator>(vehicleModel, getter);
}
}  // namespace routing
