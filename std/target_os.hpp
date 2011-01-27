#pragma once

#if defined(_BADA_SIMULATOR) || defined(_BADA_DEVICE)
  #define OMIM_OS_BADA
  #define OMIM_OS_NAME "Bada"

#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if (TARGET_OS_IPHONE > 0)
    #define OMIM_OS_IPHONE
    #define OMIM_OS_NAME "iOS"
    #if (TARGET_IPHONE_SIMULATOR > 0)
      #define OMIM_OS_IPHONE_SIMULATOR
    #else
      #define OMIM_OS_IPHONE_DEVICE
    #endif
  #else
    #define OMIM_OS_MAC
    #define OMIM_OS_NAME "MacOS"
  #endif

#elif defined(_WIN32)
  #define OMIM_OS_WINDOWS

  #ifdef __MINGW32__
    #define OMIM_OS_WINDOWS_MINGW
    #define OMIM_OS_NAME "MinGW"
  #else
    #define OMIM_OS_WINDOWS_NATIVE
    #define OMIM_OS_NAME "Win"
  #endif
#else
  #define OMIM_OS_LINUX
  #define OMIM_OS_NAME "Linux"
#endif
