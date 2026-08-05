# Comment Audit

Generated from release-relevant edited files in this SDL2 Miyoo fork. Format: `path:line:comment`.


## .gitignore

No comments found.

## Makefile.rules

2:# Build rules for objects
5:# Special dependency for SDL.c, since it depends on SDL_revision.h

## README.md

No comments found.

## build-scripts/mk_miyoo.sh

1:#!/bin/bash
31:# Defaults

## configure.ac

1:dnl Process this file with autoconf to produce a configure script.
9:dnl Save the CFLAGS to see whether they were passed in or generated
12:dnl Set various version strings - taken gratefully from the GTk sources
13:#
14:# Making releases:
15:# Edit include/SDL_version.h and change the version, then:
16:#   SDL_MICRO_VERSION += 1;
17:#   SDL_INTERFACE_AGE += 1;
18:#   SDL_BINARY_AGE += 1;
19:# if any functions have been added, set SDL_INTERFACE_AGE to 0.
20:# if backwards compatibility has been broken,
21:# set SDL_BINARY_AGE and SDL_INTERFACE_AGE to 0.
22:#
37:# libtool versioning
52:dnl Detect the canonical build and host environments
53:dnl AC_CANONICAL_HOST
55:dnl Check for tools
62:dnl Make sure that srcdir is a full pathname
65:        # Except on msys, where make can't handle full pathnames (bug 1972)
72:dnl Set up the compiler and linker flags
75:dnl Don't use our khronos headers on QNX.
84:dnl use CXX for linker on Haiku
110:        # We build SDL on cygwin without the UNIX emulation layer
127:# Uncomment the following line if you want to force SDL and applications
128:# built with it to be compiled for a particular architecture.
129:#AX_GCC_ARCHFLAG([no], [BASE_CFLAGS="$BASE_CFLAGS $ax_cv_gcc_archflag]")
131:# The default optimization for SDL is -O3 (Bug #31)
138:## These are common directories to find software packages
139:#for path in /usr/freeware /usr/pkg /usr/X11R6 /usr/local; do
140:#    if test -d $path/include; then
141:#        EXTRA_CFLAGS="$EXTRA_CFLAGS -I$path/include"
142:#    fi
143:#    if test -d $path/lib; then
144:#        EXTRA_LDFLAGS="$EXTRA_LDFLAGS -L$path/lib"
145:#    fi
146:#done
160:dnl set this to use on systems that use lib64 instead of lib
163:dnl Function to find a library in the compiler search path
181:    # Try again, this time allowing more than one version digit after the .so
191:dnl Check for compiler characteristics
196:dnl See whether we want assertions for debugging/sanity checking SDL itself.
221:dnl See whether we can use gcc style dependency tracking
230:    #if !defined(__GNUC__) || __GNUC__ < 3
231:    #error Dependency tracking requires GCC 3.0 or newer
232:    #endif
244:    dnl Skip this on platforms where it is just simply busted.
305:dnl See whether we are allowed to use the system C library
312:    dnl Check for C library headers
316:    dnl Check for typedefs, structures, etc.
319:    dnl Check for defines
322:    dnl Checks for library functions.
341:          #include <sys/types.h>
342:          #include <sys/mman.h>
357:    dnl Check for additional non-standard headers
361:dnl AC_CHECK_SIZEOF(void*)
363:dnl See whether we can use gcc atomic operations on this architecture
386:        # See if we have the minimum operation needed for GCC atomics
398:# Standard C sources
412:#SOURCES="$SOURCES $srcdir/src/filesystem/*.c"
423:dnl Enable/disable various subsystems of the SDL library
561:    # Make sure that we don't generate floating point code that would
562:    # cause illegal instruction exceptions on older processors
565:            # Don't need to worry about Apple hardware, it's all SSE capable
569:            # x86 64-bit architectures all have SSE instructions
585:    dnl Check for various instruction support
597:        #ifdef __MINGW32__
598:        #include <_mingw.h>
599:        #ifdef __MINGW64_VERSION_MAJOR
600:        #include <intrin.h>
601:        #else
602:        #include <mmintrin.h>
603:        #endif
604:        #else
605:        #include <mmintrin.h>
606:        #endif
607:        #ifndef __MMX__
608:        #error Assembler CPP flag not enabled
609:        #endif
631:        #include <mm3dnow.h>
632:        #ifndef __3dNOW__
633:        #error Assembler CPP flag not enabled
634:        #endif
660:        #ifdef __MINGW32__
661:        #include <_mingw.h>
662:        #ifdef __MINGW64_VERSION_MAJOR
663:        #include <intrin.h>
664:        #else
665:        #include <xmmintrin.h>
666:        #endif
667:        #else
668:        #include <xmmintrin.h>
669:        #endif
670:        #ifndef __SSE__
671:        #error Assembler CPP flag not enabled
672:        #endif
694:        #ifdef __MINGW32__
695:        #include <_mingw.h>
696:        #ifdef __MINGW64_VERSION_MAJOR
697:        #include <intrin.h>
698:        #else
699:        #include <emmintrin.h>
700:        #endif
701:        #else
702:        #include <emmintrin.h>
703:        #endif
704:        #ifndef __SSE2__
705:        #error Assembler CPP flag not enabled
706:        #endif
728:        #ifdef __MINGW32__
729:        #include <_mingw.h>
730:        #ifdef __MINGW64_VERSION_MAJOR
731:        #include <intrin.h>
732:        #else
733:        #include <pmmintrin.h>
734:        #endif
735:        #else
736:        #include <pmmintrin.h>
737:        #endif
738:        #ifndef __SSE2__
739:        #error Assembler CPP flag not enabled
740:        #endif
771:        #include <altivec.h>
796:            #include <altivec.h>
829:dnl See if the OSS audio interface is supported
836:    # OpenBSD "has" OSS, but it's not really for app use. They want you to
837:    #  use sndio instead. So on there, we default to disabled. You can force
838:    #  it on if you really want, though.
852:              #include <sys/soundcard.h>
859:              #include <soundcard.h>
874:            # We may need to link with ossaudio emulation library
883:dnl See if the ALSA audio interface is supported
891:        # Restore all flags from before the ALSA detection runs
922:dnl Find JACK Audio
951:                    # On Solaris, jack must be linked deferred explicitly
952:                    # to prevent undefined symbol failures.
966:dnl Find the ESD includes and libraries
1001:dnl Find Pipewire
1037:dnl Find PulseAudio
1066:                    # On Solaris, pulseaudio must be linked deferred explicitly
1067:                    # to prevent undefined symbol failures.
1098:             #include <artsc.h>
1132:dnl See if the NAS audio interface is supported
1186:dnl See if the sndio audio interface is supported
1234:dnl Find FusionSound
1273:dnl rcg07142001 See if the user wants the disk writer audio driver...
1286:dnl rcg03142006 See if the user wants the dummy audio driver...
1299:dnl See if libsamplerate is available
1333:dnl Check for ARM instruction support using gas syntax
1351:        #ifndef __ARM_EABI__
1352:        #error EABI is required (to be sure that calling conventions are compatible)
1353:        #endif
1369:dnl Check for ARM NEON instruction support using gas syntax
1389:        #ifndef __ARM_EABI__
1390:        #error EABI is required (to be sure that calling conventions are compatible)
1391:        #endif
1406:dnl See if GCC's -fvisibility=hidden is supported (gcc4 and later, usually).
1407:dnl  Details of this flag are here: http://gcc.gnu.org/wiki/Visibility
1417:    #if !defined(__GNUC__) || __GNUC__ < 4
1418:    #error SDL only uses visibility attributes in GCC 4 or newer
1419:    #endif
1429:dnl See if GCC's -fno-strict-aliasingis supported.
1430:dnl  Reference: https://bugzilla.libsdl.org/show_bug.cgi?id=4254
1449:dnl See if GCC's -mpreferred-stack-boundary is supported.
1450:dnl  Reference: http://bugzilla.libsdl.org/show_bug.cgi?id=1296
1469:dnl See if GCC's -Wdeclaration-after-statement is supported.
1470:dnl  This lets us catch things that would fail on a C89 compiler when using
1471:dnl  a modern GCC.
1490:dnl See if GCC's -Wall is supported.
1507:        dnl Haiku headers use multicharacter constants all over the place. Ignore these warnings when using -Wall.
1522:dnl Check for Wayland
1563:dnl FIXME: Do BSD and OS X need special cases?
1569:                        dnl This works in Ubuntu 13.10, maybe others
1607:dnl See if libdecor is available
1649:dnl Check for Native Client stuff
1653:          #if !defined(__native_client__)
1654:          #error "NO NACL"
1655:          #endif
1692:        # Save the original compiler flags and libraries
1695:        # Add the Raspberry Pi compiler flags and libraries
1701:          #include <bcm_host.h>
1702:          #include <EGL/eglplatform.h>
1709:        # Restore the compiler flags and libraries
1724:dnl Find the X11 include and library directories
1733:                # This isn't necessary for X11, but fixes GLX detection
1752:                    # Apple now puts this in /opt/X11
1800:                             #include <X11/Xproto.h>
1812:            # Needed so SDL applications can include SDL_syswm.h
1837:            dnl AC_CHECK_LIB(X11, XGetEventData, AC_DEFINE(SDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS, 1, [Have XGenericEvent]))
1841:                #include <X11/Xlib.h>
1957:                    #include <X11/Xlib.h>
1958:                    #include <X11/Xproto.h>
1959:                    #include <X11/extensions/XInput2.h>
1975:                # check along with XInput2.h because we use Xfixes with XIBarrierReleasePointer
1978:                    #include <X11/Xlib.h>
1979:                    #include <X11/Xproto.h>
1980:                    #include <X11/extensions/XInput2.h>
1981:                    #include <X11/extensions/Xfixes.h>]],
2008:                dnl XRRScreenResources is only present in Xrandr >= 1.2, we use that as a test.
2012:                #include <X11/Xlib.h>
2013:                #include <X11/extensions/Xrandr.h>
2107:        # Prevent Mesa from including X11 headers
2112:dnl Set up the Vivante video driver if enabled
2122:          #define LINUX
2123:          #define EGL_API_FB
2124:          #include <gc_vdk.h>
2131:          #define LINUX
2132:          #define EGL_API_FB
2133:          #include <EGL/eglvivante.h>
2151:dnl Set up the Haiku video driver if enabled
2162:dnl Set up the Cocoa video driver for Mac OS X (but not Darwin)
2170:        dnl Work around that we don't have Objective-C support in autoconf
2175:          #import <Cocoa/Cocoa.h>
2198:        dnl Work around that we don't have Objective-C support in autoconf
2203:          #import <Cocoa/Cocoa.h>
2204:          #import <Metal/Metal.h>
2205:          #import <QuartzCore/CAMetalLayer.h>
2207:          #if TARGET_CPU_X86
2208:          #error Metal doesn't work on this configuration
2209:          #endif
2227:dnl Find DirectFB
2237:            # SuSE 11.1 installs directfb-config without directfb-devel
2277:dnl Find KMSDRM
2335:dnl rcg04172001 Set up the Null video driver.
2362:dnl Set up the QNX video driver if enabled
2374:dnl Set up the QNX audio driver if enabled
2413:dnl Check to see if OpenGL support is desired
2418:dnl Find GLX
2425:         #include <GL/glx.h>
2435:dnl Check to see if OpenGL ES support is desired
2446:dnl Find EGL
2453:          #define LINUX
2454:          #define EGL_API_FB
2455:          #define MESA_EGL_NO_X11_HEADERS
2456:          #define EGL_NO_X11
2457:          #include <EGL/egl.h>
2458:          #include <EGL/eglext.h>
2467:dnl Find OpenGL
2474:         #include <GL/gl.h>
2475:         #include <GL/glext.h>
2486:dnl Find OpenGL ES
2494:             #include <GLES/gl.h>
2495:             #include <GLES/glext.h>
2509:             #include <GLES2/gl2.h>
2510:             #include <GLES2/gl2ext.h>
2522:dnl Check for Windows OpenGL
2533:dnl Check for Windows OpenGL
2541:         #include <EGL/egl.h>
2553:         #include <GLES2/gl2.h>
2554:         #include <GLES2/gl2ext.h>
2566:dnl Check for Haiku OpenGL
2578:dnl Check for MacOS OpenGL
2589:dnl Check for MacOS OpenGLES
2608:         #include <EGL/egl.h>
2618:         #include <GLES2/gl2.h>
2619:         #include <GLES2/gl2ext.h>
2630:dnl Check to see if Vulkan support is desired
2635:dnl Find Vulkan Header
2642:                  #if defined(__ARM_ARCH) && __ARM_ARCH < 7
2643:                  #error Vulkan doesn't work on this configuration
2644:                  #endif
2649:                dnl Work around that we don't have Objective-C support in autoconf
2652:                  #include <Cocoa/Cocoa.h>
2653:                  #include <Metal/Metal.h>
2654:                  #include <QuartzCore/CAMetalLayer.h>
2656:                  #if TARGET_CPU_X86
2657:                  #error Vulkan doesn't work on this configuration
2658:                  #endif
2666:            # For reasons I am totally unable to see, I get an undefined macro error if
2667:            # I put this in the AC_TRY_COMPILE.
2681:dnl See if we can use the new unified event interface in Linux 2.4
2684:    dnl Check for Linux 2.4 unified input event interface support
2688:          #include <linux/input.h>
2690:          #ifndef EVIOCGNAME
2691:          #error EVIOCGNAME() ioctl not available
2692:          #endif
2701:dnl See if we can use the kernel kd.h header
2708:      #include <linux/kd.h>
2709:      #include <linux/keyboard.h>
2722:dnl See if we can use the FreeBSD kernel kbio.h header
2728:      #include <sys/kbio.h>
2729:      #include <sys/ioctl.h>
2741:dnl See if we can use the wscons input driver
2747:     #include <sys/time.h>
2748:     #include <dev/wscons/wsconsio.h>
2749:     #include <dev/wscons/wsksymdef.h>
2750:     #include <dev/wscons/wsksymvar.h>
2751:     #include <sys/ioctl.h>
2763:dnl See if the platform offers libudev for device enumeration and hotplugging.
2785:dnl See if the platform offers libdbus for various IPC techniques.
2807:dnl See if the platform wanna IME support.
2819:dnl Check inotify presense
2841:dnl See if the platform has libibus IME support.
2874:dnl See if the platform has fcitx IME support.
2896:dnl Check to see if GameController framework support is desired
2906:        dnl Work around that we don't have Objective-C support in autoconf
2912:          #include <AvailabilityMacros.h>
2913:          #include <TargetConditionals.h>
2914:          #import <GameController/GameController.h>
2916:          #if MAC_OS_X_VERSION_MIN_REQUIRED < 1080
2917:          #error GameController framework doesn't work on this configuration
2918:          #endif
2919:          #if TARGET_CPU_X86
2920:          #error GameController framework doesn't work on this configuration
2921:          #endif
2935:dnl See what type of thread model to use on Linux and Solaris
2938:    dnl Check for pthread support
2940:    dnl Emscripten pthreads work, but you need to have a non-pthread fallback build
2941:    dnl  for systems without support. It's not currently enough to not use
2942:    dnl  pthread functions in a pthread-build; it won't start up on unsupported
2943:    dnl  browsers. As such, you have to explicitly enable it on Emscripten builds
2944:    dnl  for the time being. This default with change to ON once this becomes
2945:    dnl  commonly supported in browsers or the Emscripten teams makes a single
2946:    dnl  binary work everywhere.
2960:    dnl This is used on Linux for glibc binary compatibility (Doh!)
2987:# causes Carbon.p complaints?
2988:#            pthread_cflags="-D_REENTRANT -D_THREAD_SAFE"
3003:            # From Solaris 9+, posix4's preferred name is rt.
3008:            # Solaris 10+ merged pthread into libc.
3013:            # Solaris 11+ merged rt into libc.
3047:        # Save the original compiler flags and libraries
3049:        # Add the pthread compiler flags and libraries
3051:        # Check to see if we have pthread support on this system
3055:         #include <pthread.h>
3061:        # Restore the compiler flags and libraries
3064:        # Do futher testing if we have pthread support...
3071:            # Save the original compiler flags and libraries
3073:            # Add the pthread compiler flags and libraries
3076:            # Check to see if recursive mutexes are available
3081:                  #define _GNU_SOURCE 1
3082:                  #include <pthread.h>
3093:                  #define _GNU_SOURCE 1
3094:                  #include <pthread.h>
3105:            # Check to see if pthread semaphore support is missing
3110:                  #include <pthread.h>
3111:                  #include <semaphore.h>
3119:                  #include <pthread.h>
3120:                  #include <semaphore.h>
3135:            # Check to see if pthread naming is available
3154:            # Restore the compiler flags and libraries
3157:            # Basic thread creation functions
3160:            # Semaphores
3161:            # We can fake these with mutexes and condition variables if necessary
3168:            # Mutexes
3169:            # We can fake these with semaphores if necessary
3172:            # Condition variables
3173:            # We can fake these with semaphores and mutexes if necessary
3176:            # Thread local storage
3184:dnl Determine whether the compiler can produce Windows executables
3190:     #include <windows.h>
3202:#if !defined(_WIN32_WCE) && !defined(__MINGW32CE__)
3203:#error This is not Windows CE
3204:#endif
3213:    # This fixes Windows stack alignment with newer GCC
3217:dnl Determine whether the compiler can produce OS/2 executables
3232:dnl Find the DirectX includes and libraries
3259:        # FIXME: latest Cygwin finds dinput headers, but we die on other win32 headers.
3260:        # FIXME:  ...so force it off for now.
3277:#include <windows.h>
3278:#include <xinput.h>
3284:#include <windows.h>
3285:#include <xinput.h>
3303:#define COBJMACROS
3304:#include <windows.gaming.input.h>
3327:dnl Check for the dlfcn.h interface for dynamically loading objects
3328:dnl NOTE: CheckDLOPEN is called only for relevant platforms
3355:#include <fcntl.h>
3364:dnl Check for the usbhid(3) library on *BSD
3387:                  #include <sys/types.h>
3388:                  #if defined(HAVE_USB_H)
3389:                  #include <usb.h>
3390:                  #endif
3391:                  #ifdef __DragonFly__
3392:                  # include <bus/u4b/usb.h>
3393:                  # include <bus/u4b/usbhid.h>
3394:                  #else
3395:                  # include <dev/usb/usb.h>
3396:                  # include <dev/usb/usbhid.h>
3397:                  #endif
3398:                  #if defined(HAVE_USBHID_H)
3399:                  #include <usbhid.h>
3400:                  #elif defined(HAVE_LIBUSB_H)
3401:                  #include <libusb.h>
3402:                  #elif defined(HAVE_LIBUSBHID_H)
3403:                  #include <libusbhid.h>
3404:                  #endif
3416:                      #include <sys/types.h>
3417:                      #if defined(HAVE_USB_H)
3418:                      #include <usb.h>
3419:                      #endif
3420:                      #ifdef __DragonFly__
3421:                      # include <bus/u4b/usb.h>
3422:                      # include <bus/u4b/usbhid.h>
3423:                      #else
3424:                      # include <dev/usb/usb.h>
3425:                      # include <dev/usb/usbhid.h>
3426:                      #endif
3427:                      #if defined(HAVE_USBHID_H)
3428:                      #include <usbhid.h>
3429:                      #elif defined(HAVE_LIBUSB_H)
3430:                      #include <libusb.h>
3431:                      #elif defined(HAVE_LIBUSBHID_H)
3432:                      #include <libusbhid.h>
3433:                      #endif
3446:                      #include <sys/types.h>
3447:                      #if defined(HAVE_USB_H)
3448:                      #include <usb.h>
3449:                      #endif
3450:                      #ifdef __DragonFly__
3451:                      #include <bus/u4b/usb.h>
3452:                      #include <bus/u4b/usbhid.h>
3453:                      #else
3454:                      #include <dev/usb/usb.h>
3455:                      #include <dev/usb/usbhid.h>
3456:                      #endif
3457:                      #if defined(HAVE_USBHID_H)
3458:                      #include <usbhid.h>
3459:                      #elif defined(HAVE_LIBUSB_H)
3460:                      #include <libusb.h>
3461:                      #elif defined(HAVE_LIBUSBHID_H)
3462:                      #include <libusbhid.h>
3463:                      #endif
3476:                      #include <machine/joystick.h>
3498:dnl Check for HIDAPI joystick drivers
3510:            # libusb does not support iOS
3514:            # On the other hand, *BSD specifically uses libusb only
3545:                    # libusb is loaded dynamically, so don't add it to LDFLAGS
3576:dnl Check for clock_gettime()
3597:dnl Check for a valid linux/version.h
3606:dnl Check if we want to use RPATH
3614:dnl Check if we want to use custom signals to fake iOS/Android's backgrounding
3615:dnl  events. These could be useful if you're building a custom embedded
3616:dnl  environment, etc, but most people don't need this.
3634:dnl Set up the Virtual joystick driver.
3647:dnl Do this on all platforms, before everything else (other things might want to override it).
3651:dnl Do this for every platform, but for some it doesn't mean anything, but better to catch it here anyhow.
3656:dnl Set up the configuration based on the host platform!
3661:                # Android
3723:        # Need to check for Raspberry PI first and add platform specific compiler flags, otherwise the test for GLES fails!
3727:        # Need to check for EGL first because KMSDRM and Wayland depends on it.
3767:        # Set up files for the audio library
3811:        # Set up files for the joystick library
3841:        # Set up files for the haptic library
3858:        # Set up files for the sensor library
3868:        # Set up files for the power library
3883:        # Set up files for the filesystem library
3898:        # Set up files for the timer library
3904:        # Set up files for udev hotplugging support
3908:        # Set up files for evdev input
3914:        # Set up files for wscons input
3919:        # Set up other core UNIX files
3927:            # Default cross-compile location
3930:            # Look for the location of the tools and install there
3950:        # Set up the core platform files
3954:        # Use the Windows locale APIs.
3958:        # Set up files for the video library
3975:        # Set up files for the audio library
3992:        # Set up files for the joystick library
4026:        # Set up files for the sensor library
4036:        # Set up files for the power library
4042:        # Set up files for the filesystem library
4048:        # Set up files for the thread library
4056:        # Set up files for the timer library
4062:        # Set up files for the shared object loading library
4067:        # Set up the system libraries we need
4075:        # The Windows platform requires special setup
4081:        # Check to see if this is a mingw or cygwin build
4091:    dnl BeOS support removed after SDL 2.0.1. Haiku still works.  --ryan.
4111:        # Set up files for the audio library
4118:        # Set up files for the joystick library
4124:        # Set up files for the timer library
4130:        # Set up files for the system power library
4136:        # Set up files for the system filesystem library
4146:        # Set up files for the locale library
4150:        # The Haiku platform requires special setup.
4172:        # Set up files for the locale library
4176:        # Set up files for the audio library
4183:        # Set up files for the joystick library
4190:            # Need this code for accelerometer as joystick support
4193:        # Set up files for the haptic library
4194:        #if test x$enable_haptic = xyes; then
4195:        #    SOURCES="$SOURCES $srcdir/src/haptic/darwin/*.c"
4196:        #    have_haptic=yes
4197:        #    EXTRA_LDFLAGS="$EXTRA_LDFLAGS -Wl,-framework,ForceFeedback"
4198:        #fi
4199:        # Set up files for the sensor library
4205:        # Set up files for the power library
4211:        # Set up files for the filesystem library
4216:        # Set up additional files for the file library
4221:        # Set up files for the timer library
4227:        # Set up other core UNIX files
4229:        # The iOS platform requires special setup.
4253:        # This could be either full "Mac OS X", or plain "Darwin" which is
4254:        # just the OS X kernel sans upper layers like Carbon and Cocoa.
4255:        # Next line is broken, and a few files below require Mac OS X (full)
4258:        # Mac OS X builds with both the Carbon and OSX APIs at the moment
4284:        # Set up files for the locale library
4288:        # Set up files for the audio library
4296:        # Set up files for the joystick library
4304:        # Set up files for the haptic library
4311:        # Set up files for the power library
4317:        # Set up files for the filesystem library
4323:        # Set up files for the timer library
4329:        # Set up additional files for the file library
4333:        # Set up other core UNIX files
4335:        # The Mac OS X platform requires special setup.
4356:        # Set up files for the timer library
4395:         # Set up files for the power library
4402:        # Set up files for the power library
4409:        # Set up files for the filesystem library
4415:        # Set up files for the timer library
4421:        # Set up files for the locale library
4442:        # Set up files for the video library
4449:        # Set up files for the filesystem library
4455:        # Set up files for the timer library
4465:            # Default cross-compile location
4468:            # Look for the location of the tools and install there
4482:        # Set up the core platform files
4487:        # Use the Unix locale APIs.
4490:        # Set up files for the video library
4497:        # Set up files for the audio library
4505:        # Set up files for the thread library
4512:        # Set up files for the timer library
4518:        # Set up files for the shared object loading library
4524:        # Set up files for the filesystem library
4530:        # Set up files for the joystick library
4544:dnl Permit use of virtual joystick APIs on any platform (subject to configure options)
4547:# Check whether to install sdl2-config
4563:# Verify that we have all the platform specific files we need
4675:# Set runtime shared library paths as needed
4701:dnl Expand the cflags and libraries needed by apps using SDL
4730:dnl Expand the sources and objects needed to build the library
4748:# Build rules for objects
4751:# Special dependency for SDL.c, since it depends on SDL_revision.h

## src/audio/mmiyoo/SDL_audio_mmiyoo.c

1:/*
23:*/

## src/audio/mmiyoo/SDL_audio_mmiyoo.h

1:/*
24:*/
40:    /* The file descriptor for the audio device */
43:    /* Raw mixing buffer */
52:#define FUDGE_TICKS 10      /* The scheduler overhead ticks per frame */

## src/joystick/mmiyoo/SDL_joystick_mmiyoo.c

1:/*
24:*/

## src/render/mmiyoo/SDL_render_mmiyoo.c

1:/*
23:*/
95:    // Fence batching system
100:    // Color state for draw operations
106:    // Track if texture was blitted to framebuffer this frame
109:    // Optional geometry instrumentation (enabled via SDL_MMIYOO_GEOMETRY_STATS hint)
288:            if (outcodeOut & 8) { /* TOP */
291:            } else if (outcodeOut & 4) { /* BOTTOM */
294:            } else if (outcodeOut & 2) { /* RIGHT */
297:            } else { /* LEFT */
315:// Optimized edge structure for incremental rasterization
317:    float x_current;    // Current X intersection (for horizontal scanning)
318:    float y_current;    // Current Y intersection (for vertical scanning)
319:    float dx_dy;        // X increment per Y step (slope)
320:    float dy_dx;        // Y increment per X step (inverse slope)
321:    int y_min, y_max;   // Y range where edge is active
322:    int x_min, x_max;   // X range where edge is active
323:    SDL_bool active;    // Whether edge is active for current scanline
326:// Setup edge data for incremental walking - called once per triangle
335:    // Handle horizontal edges (still need Y range for boundary checking)
345:    // Handle vertical edges (still need X range for boundary checking)
357:    // Calculate slopes once - this is where we save all the performance!
361:    // Set Y range properly - always use the correct ordering
372:    // Set X range for boundary checking
384:/* Legacy intersection helpers removed: optimized rasterizer no longer uses them. */
386:// Optimized triangle filling using incremental edge walking
744:    /* Apply coordinate flipping for framebuffer targets (physically upside-down display) */
759:        add_fence_to_batch(data, fence);  // Use optimized batching
869:        add_fence_to_batch(data, fence);  // Use optimized batching
877:// Convert SDL pixel format to MI_SYS frame format
902:// Advanced DMA operations using MI_SYS_BufBlitPa for MI_SYS-to-MI_SYS transfers
934:    // Set up source rectangle
940:    // Set up destination rectangle  
946:    // Perform hardware-accelerated blit using DMA
950:// Forward declaration for batching functions
1093:// Optimized fence batching functions for performance
1098:        // Wait for all fences in batch - more efficient than individual waits
1106:// Optimized batch management: flush when full and continue batching
1111:            flush_batch(data);  // Flush full batch
1115:        MI_GFX_WaitAllDone(FALSE, fence);  // Immediate wait when batching disabled
1124:// Convert SDL pixel format to MI_GFX format with comprehensive validation
1145:            return E_MI_GFX_FMT_ARGB8888;  /* Force ARGB8888 for consistency with framebuffer */
1282:    /* Ensure any pending hardware operations touching this texture are completed before we overwrite it. */
1326:        // Cache flush optimization: only flush modified region, not entire texture
1388:    /* Reset clip tracking when target changes to avoid stale rectangles */
1437:// Forward declaration
1761:    // DMA optimization: if both source and target are MI_SYS textures, use hardware blit
1776:        /* Framebuffer draws need to counter the upside-down panel */
1788:    /* Enhanced logging for overlay texture operations */
2411:    /* Force SDL to prefer hardware line rendering when no explicit hint is set. */
2534:    // Initialize viewport to full screen
2543:    // Initialize fence batching system
2545:    data->batching_enabled = SDL_TRUE;  // Enable batching by default
2547:    // Initialize draw color to white (default SDL behavior)

## src/video/mmiyoo/SDL_event_mmiyoo.c

1:/*
24:*/
90:                    //printf("%s, code:%d\n", __func__, ev.code);

## src/video/mmiyoo/SDL_event_mmiyoo.h

1:/*
22:*/
53:#define MYKEY_LAST_BITS     18 // ignore POWER, VOL-, VOL+ keys

## src/video/mmiyoo/SDL_framebuffer_mmiyoo.c

1:/*
23:*/

## src/video/mmiyoo/SDL_framebuffer_mmiyoo.h

1:/*
23:*/

## src/video/mmiyoo/SDL_opengles_mmiyoo.c

1:/*
23:*/
38:// EGLBoolean eglUpdateBufferSettings(EGLDisplay display, EGLSurface surface, void *pFunc, void *fb_idx, void *fb_vaddr);
56:    (void)name; /* GLES library is provided by the platform. */
58:    /* Cache optional extension entry points we care about. */
99:    (void)window; /* Miyoo does not expose native window handles. */

## src/video/mmiyoo/SDL_opengles_mmiyoo.h

1:/*
23:*/

## src/video/mmiyoo/SDL_video_mmiyoo.c

1:/*
23:*/
126:// Global texture fence management for performance
143:// Flush all pending texture fences for performance
175:    // Reset signal to default and re-raise to get core dump
185:        // Wait for action signal instead of polling
415:    // Initialize SDL synchronization primitives
452:    // Create SDL thread instead of pthread
463:    // Signal the thread to wake up and exit
470:    // Wait for thread to complete
480:    // Properly cleanup MI_SYS allocated memory
493:    // Cleanup SDL synchronization primitives
503:    // Single buffer mode - no yoffset to reset
636:    /* Validate format consistency */
665:        /* Ensure previous hardware blits that consumed the shared staging buffer
671:        aligned_stride = (row_bytes + 15) & ~15;  /* 16-byte alignment for MI_GFX */
679:        /* Validate copy size */
685:        /* Pure MI_SYS strategy: external pixel data always uses neon_memcpy to MI_SYS staging buffer */
686:        /* DMA optimization happens between MI_SYS buffers in hardware operations */
694:            dst_row += aligned_stride;  /* Use aligned stride for staging buffer */
700:    /* Configure blending according to SDL's requested mode */
706:    /* Disable colorkey operations for predictable blending */
707:    gfx.hw.opt.stSrcColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */
708:    gfx.hw.opt.stDstColorKeyInfo.bEnColorKey = 0;  /* MI_FALSE */
710:    /* Blend-factor mapping follows SigmaStar docs (see
711:     *   GFX - SigmaStarDocs copy.txt §3.9/3.10 for DfbBldOp_e and DfbBlendFlags_e
712:     * and SDL's composed modes in src/render/SDL_render.c). */
735:            /* SDL_BLENDMODE_BLEND and any unknown modes fall back to standard alpha blending */
742:    /* Apply clipping if enabled; incoming clip rect is already in target coordinate space */
749:        /* No clipping - set to full target area */
762:    /* Setup source surface and rectangle */
766:        gfx.hw.src.rt.s32Xpos = 0;  /* Always 0 for staging buffer */
767:        gfx.hw.src.rt.s32Ypos = 0;  /* Always 0 for staging buffer */
772:        gfx.hw.src.surf.u32Stride = (srcrect.w * src_bytes_per_pixel + 15) & ~15;  /* 16-byte aligned stride */
781:        (void)cpu_src; /* currently unused but kept for future cache management */
793:    /* Setup destination rectangle using renderer-space coordinates */
802:        /* Render directly to the active framebuffer (front or back) */
810:    /* Validate memory addresses, dimensions and stride alignment before BitBlit */
818:    /* Validate stride alignment (MI_GFX requires 16-byte alignment) */
839:        // Add fence to global batch instead of waiting immediately
851:// Framebuffer helpers

## src/video/mmiyoo/SDL_video_mmiyoo.h

1:/*
23:*/
101:    // SDL threading synchronization
127:// Single/double buffer management

## docs/HANDOVER.md

No comments found.

## docs/TEST_PLAN.md

No comments found.
