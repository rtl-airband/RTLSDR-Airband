# NEWS

This file will no longer be updated with each release, for changes between releases, see PRs merged to the repo

Version 5.0.0 (Jan 21, 2024):

* NOTE: Going forward a release tag will be automatically created on each merge to `main`, and changes will not be reflected in this file.  For changes between versions see the repo's [release history](https://github.com/rtl-airband/RTLSDR-Airband/releases).
* NOTE: Going forward PRs will be opened directly against `main` and the `unstable` branch will no longer be used.
* NOTE: This repo has significantly diverged from the original project [microtony/RTLSDR-Airband](https://github.com/microtony/RTLSDR-Airband) so it has been been detached (ie no longer a fork).

* Changes in this release, see [#444](https://github.com/rtl-airband/RTLSDR-Airband/pull/444):
  * build and publish docker containers
  * changes to supported `cmake` platforms:
    * depreciate `rpiv1`, `armv7-generic`, and `armv8-generic` and build platforms
    * change default build platform to `native`
    * rename `default` to `generic`
  * enable a series of compile warnings and cleanup code
  * remove `SSE` specific code - let the compiler "do the right thing"
  * remove some no longer supported windows `ifdef`'s
  * fix CTCSS bug that could miss a tone when multiple tones have the same power (happens with less accurate floating point operations, ie i386)

Version 4.2.0 (Oct 13, 2023):

* Changes in this release:
  * Add support for building with libshout v2.4.6, see [#382](https://github.com/rtl-airband/RTLSDR-Airband/pull/382) and [#422](https://github.com/rtl-airband/RTLSDR-Airband/pull/422)
  * Add error checking for lowpass <= highpass, see [#399](https://github.com/rtl-airband/RTLSDR-Airband/pull/399) and [#412](https://github.com/rtl-airband/RTLSDR-Airband/pull/412)
  * Remove limit on count of mixer inputs (thanks @cdknox), see [#408](https://github.com/rtl-airband/RTLSDR-Airband/pull/408)
  * Add `dated_subdirectories` config option for output files (thanks, @marcin-osowski), see [#413](https://github.com/rtl-airband/RTLSDR-Airband/pull/413)

Version 4.1.1 (May 1, 2023):

* Changes in this release:
  * Fix build issues when using VideoCore GPU, see [#378](https://github.com/rtl-airband/RTLSDR-Airband/pull/378)

Version 4.1.0 (April 23, 2023):

* Changes in this release:
  * Add `channel_dbfs_noise_level` and `channel_dbfs_signal_level` to the stats file, see [#355](https://github.com/rtl-airband/RTLSDR-Airband/pull/355)
  * Add squelch support for CTCSS, add `channel_ctcss_counter` and `channel_no_ctcss_counter` to the stats file, see [#368](https://github.com/rtl-airband/RTLSDR-Airband/pull/368)
  * Support `ampfactor` on a per-channel basis (in addition to mixer inputs), see [#369](https://github.com/rtl-airband/RTLSDR-Airband/pull/369)
  * Fix config error messages, see [#371](https://github.com/rtl-airband/RTLSDR-Airband/pull/371)
  * Multiple CI / workflow improvements, including:
    * Addition of Dockerfiles and shell scripts for multiple build environments
    * Addition of vscode devcontainer configuration
    * Addition of gtest, code refactoring, addition of unit tests, running unit tests on each pull request
    * Running more combinations of OSs, build types, and build options on each pull request

Version 4.0.3 (Jan 10, 2023):

* Changes in this release:
  * Add `channel_squelch_level` to stats file, see [#332](https://github.com/rtl-airband/RTLSDR-Airband/pull/332)
  * Support "default" values in lists for `squelch_snr_threshold` and `notch_q`,
   see [#334](https://github.com/rtl-airband/RTLSDR-Airband/pull/334)
  * Set cmake `ENABLE_EXPORTS` property, see [#339](https://github.com/rtl-airband/RTLSDR-Airband/pull/339)

* Other items to note:
  * Repo maintainer has changed, see [#342](https://github.com/rtl-airband/RTLSDR-Airband/discussions/342)
  * Repo URL has moved to https://github.com/rtl-airband/RTLSDR-Airband
  * Default branch / Top of Tree has been renamed to `main`

Version 4.0.2 (Dec 26, 2021):

* Added a new `PLATFORM` value `default` (which, as the name says, is the new
  default). It results in a portable binary without any architecture-specific
  optimizations. This also allows the program to be built with compilers that
  do not support `-march=native` option (notably Clang on Apple M1) (#303).

Version 4.0.1 (Nov 14, 2021):

* Fixed compilation error on RaspberryPi OS 11 (Bullseye)

Version 4.0.0 (Oct 19, 2021):

* RTLSDR-Airband is now built with CMake. Refer to the wiki for updated
  compilation instructions.
* When compiling the program, a new `PLATFORM` value `native` can now be
  specified. It enables `-march=native -mtune=native` compilation options. This
  causes the compiler to apply the most appropriate optimizations for the
  hardware on which the app is being built (thx @charlie-foxtrot).
* BACKWARDS-INCOMPATIBLE CHANGE: Signal level and noise level estimates
  displayed in the textual waterfalls are now expressed in dBFS (decibels
  related to the full scale of the analog-to-digital converter). The main
  benefit of the new approach is that these values do not depend on the
  `fft_size` value(thx @charlie-foxtrot).
* BACKWARDS-INCOMPATIBLE CHANGE: Improved squelch algorithm with new
  configuration parameters. `squelch` keyword has been replaced with
  `squelch_threshold` which takes an absolute signal value in dBFS as an
  argument. Alternatively, a minimum signal-to-noise ratio (in dB) that should
  trigger the squelch might be configured using `squelch_snr_threshold` option
  (thx @charlie-foxtrot).
* BACKWARDS-INCOMPATIBLE CHANGE: `include_freq` config option for file outputs
  now causes the frequency to be appended after the timestamp rather than
  before it. This feature now works correctly in scan mode, when
  `split_on_transmission` feature is enabled. (thx @charlie-foxtrot).
* BACKWARDS-INCOMPATIBLE CHANGE: sample format in files produced by `rawfile`
  outputs has been changed from CS16 to CF32. File name suffix is now `.cf32`.
* Improved squelch indicator in the textual waterfalls. In addition to the `*`
  character indicating that the squelch is open, there is also a `~` character
  indicating that the channel has a signal that is being suppressed because it
  is outside the band of the channel filter (thx @charlie-foxtrot).
* New output type `udp_stream` for sending uncompressed audio to another host
  via UDP/IP (thx @charlie-foxtrot).
* Added `multiple_output_threads` global option. When set to `true`, a separate
  output thread is spawned for each device (thx @charlie-foxtrot).
* Modulation in scan mode is now configurable per channel (thx
  @charlie-foxtrot).
* SoapySDR errors like TIMEOUT or OVERFLOW are no longer treated as fatal. They
  often appear intermittently, especially when the CPU usage is high. There is
  no point in failing the input in this case.
* Added `.tmp` suffix to the names of the output files currently being written
  to. The suffix is removed when the file is closed. External applications that
  consume recorded files can now figure out which files are not yet complete.
* Added logging and statistics for output thread overruns and mixer
  input/output overruns (thx @charlie-foxtrot).
* The program can now be built on MacOS.
* Miscellaneous bug fixes and code cleanups.

Version 3.2.1 (Nov 13, 2020):

* Fixed a compile error when using libshout older than 2.4.0

Version 3.2.0 (Nov 08, 2020):

* Added `split_on_transmission` output file option which allows creating
  a new file for every transmission on the channel (thx @charlie-foxtrot).
* Added `include_freq` output file option, which causes the channel frequency
  to be appended to the file name (thx @charlie-foxtrot).
* Added support for notch filters for eliminating narrowband interference,
  like CTCSS tones (thx @charlie-foxtrot).
* Added `bandwidth` channel option which causes the channelized I/Q signal
  to be lowpass-filtered before demodulation. This might help in situations
  where neighboring channels are closely spaced and interfere with the channel
  of interest. It also reduces the bandwidth of the resulting audio signal,
  and thus eliminates the high-frequency noise (thx @charlie-foxtrot).
* Added support for multithreaded demodulation. Each device can now have its
  own demodulation thread. This allows spreading the demodulation work across
  multiple CPU cores. Enable with `multiple_demod_threads` global option
  (thx @charlie-foxtrot).
* Added support for highpass/lowpass MP3 filters for mixers (thx @charlie-foxtrot)
* Added support for frequency usage statistics (thx @charlie-foxtrot).
* Workaround for Fitipower tuner problem of not honoring the first gain
  setting when the device is first used (thx @eshaz).
* Finalize the MP3 file properly before opening a new one (thx @jratke).
* Close the RTL device properly on program exit (thx @jratke).
* Updated the SoapySDR input driver to reflect changes in SoapySDR library API.
* Minor cleanups.

Version 3.1.0 (Jan 19, 2020):

* SoapySDR: added support for complex float 32-bit samples
* SoapySDR: allow using AGC if the device supports it. Gain setting for
  soapy devices is now optional - if it's not specified, the program will
  try to enable AGC.
* Use lowpass/highpass filters provided by LAME library to improve audio
  quality of MP3 streams. Filter cutoff frequencies may be configured per
  output, using `highpass` and `lowpass` config options. Credit: clydebarrow.
* Added `log_scan_activity` global config option. When set to `true`, a
  log message is written whenever a squelch opens on a scanned channel,
  effectively producing a channel activity log. Credit: clam-i-am.
* Improved squelch behaviour in some corner cases.
* Fix for incorrect naming of pulseaudio context. Name set in the config
  was not used as it should. Credit: Darryl Pogue.
* Don't fail when the configured gain value is negative. Some SDRs support
  this (eg. FC0012-based dongles).
* Fix a bug which in some cases could prevent the icecast output from
  reconnecting with the Icecast server after the connection has failed.

Version 3.0.1 (Feb 16, 2018):

* Fix for squelch staying constantly open when configured manually
  with NFM=off (#84)

Version 3.0.0 (Feb 10, 2018):

* Major overhaul of the SDR input code - now it's modular and
  hardware-agnostic (no longer tightly coupled with librtlsdr).
* Support for SoapySDR vendor-neutral SDR library - any SDR which has
  a plugin for SoapySDR shall now work in RTLSDR-Airband.
* Support for Mirics DVB-T dongles via libmirisdr-4 library.
* Support for RTLSDR is now optional and can be disabled at compilation
  stage.
* Removed the 8-channels-per-device limit in multichannel mode.
* Configurable per-device sampling rate.
* Configurable FFT size.
* Support for multibyte input samples.
* Support for rawfile outputs (ie. writing raw I/Q data from a
  narrowband channel to a file for processing with other programs,
  line GNUradio or csdr).
* INCOMPATIBLE CHANGE: removed `rtlsdr_buffers` global configuration
  option; buffer count can now be adjusted with a per-device
  "buffers" option.
* INCOMPATIBLE CHANGE: removed `syslog` global configuration option;
  syslog logging is now enabled by default, both in foreground and
  background mode. To force logging to standard error, use -e command
  line option.
* Added -F command line option for better cooperation with systemd.
  Runs the program in foreground, but without textual waterfalls.
  Together with -e it allows running rtl_airband as a service of type
  "simple" under systemd. Example rtl_airband.service file has been
  adjusted to reflect this change.
* Added `type` device configuration option. It sets the device type
  (ie. the input driver which shall be used to talk to the device).
  "rtlsdr" is assumed as a default type for backward compatibility.
  If RTLSDR support has been disabled at compilation stage, then
  there is no default type - it must be set manually, or the program
  will throw an error on startup.
* Frequencies in the config can now be expressed in Hz, kHz, MHz or GHz
  for improved readability.
* Lots of bugfixes.
* Rewritten documentation on [Github Wiki](https://github.com/rtl-airband/RTLSDR-Airband/wiki).

Version 2.4.0 (Oct 15, 2017):

* Support for PulseAudio output via new output type `pulse`. With this
  feature you can eg. play the sound via the soundcard of the Raspberry
  Pi you run RTLSDR-Airband on (you need to install and run pulseaudio
  daemon on it, though). Or you can stream the audio from a Pi located
  near the antenna (eg. in the attic) to speakers connected to the desktop
  PC you are sitting at, without launching a local Icecast server,
  as before. Because the audio stream is sent uncompressed, it is
  not recommended to run it across the Internet - jitter or packet loss
  will easily cause the audio to become choppy. However in a local network
  PulseAudio is a good choice. And it gives much lower latency as compared
  to Icecast (typically under 0.5 seconds). Thanks to Marcus Ströbel
  for the idea and initial implementation.
* Support for referring to RTL devices by their serial numbers in the
  config file. Instead of `index = <dongle_index>` parameter, use `serial =
  <dongle_serial_number>` to get consistent behavior across reboots
  and hardware reconfigurations.
* Set RTL gain to the nearest gain value supported by the device. This is
  required for E4000 tuners, which do not round the given gain value to
  the nearest supported setting, which causes the gain setting operation
  to fail.
* Improved squelch operation in scan mode. All squelch-related variables
  (noise floor, AGC coefficients, etc) are now calculated and stored
  separately for each scanned channel. Earlier their values were common
  to all channels, which caused squelch problems in case when noise floor
  varied considerably between channels. Thanks to @strix-technica.
* Added build target for FreeBSD on x86. Use `PLATFORM=x86-freebsd` to
  compile and `PLATFORM=x86-freebsd gmake install` to install. Thanks
  to @nyammy.
* Display squelch setting in waterfall in place of noise floor value when
  squelch is set manually.
* Bug fixes, performance improvements.
* Decluttered and more understandable documentation.

Version 2.3.0 (Jan 2, 2017):

* Added support for mixers. It is now possible to produce audio streams
  combined from several input channels. Both mono and stereo mixing is
  supported. Usage example is provided in config/mixers.conf. All
  mixer-related parameters are documented in config/reference.conf.
* Added build options for 64-bit ARM architectures, like Odroid C2.
  Please use PLATFORM=armv8-generic when compiling.
* Fixed a long-standing bug in RTL sample processing, which caused some
  samples to be processed twice. If you were annoyed by these regular
  clicks in NFM audio every 125 ms, they are now gone.
* Reduced CPU usage on x86
* Some code restructuring and cleanups
* Added several configuration file examples for typical real-life
  scenarios. They are placed in config/ subdirectory. rtl_airband.conf.example
  file has been moved to config/reference.conf. It is meant to be a reference
  for all supported config knobs together with their description. This is
  still an interim solution before some more readable and understandable
  documentation gets written.

Version 2.2.0 (Oct 8, 2016):

* Support for Icecast stream metadata updates in scanning mode. When enabled,
  every time the scanner stops on a channel, current frequency is written into
  Icecast song title, which in turn is displayed in the player. Alternatively,
  textual labels can be configured for each frequency. It is possible
  to configure the amount of delay between the stream and metadata updates to
  synchronize them with the audio. There are some caveats however - read
  comments in rtl_airband.conf.example for details.
* Added global option 'localtime'. When enabled, rtl_airband uses local time
  instead of UTC time for output file names. (Credit: ScanOC).
* Auto gain feature removed. RTL auto gain does not work well for narrowband
  channels. Most often it sets the gain too high which causes problems for
  auto squelch and audio bleeding between adjacent channels. Gain must be
  configured manually from now on.
* Dropped unmaintained Windows build.
* Reverted to power level calculation algorithm from version 2.0.2. The new
  algo didn't really do much to sensitivity, but introduced annoying clicks
  on squelch open/close.
* Improved DC offset estimator for AM mode. This one hardly ever clicks
  on squelch opening.
* Boosted AM audio volume.
* Reduced squelch flapping in NFM mode.

Version 2.1.0 (Aug 11, 2016):

* Narrowband FM demodulation support
* Automatic Frequency Control
* Append mode for recording (enabled by default)
* Dongles, channels and outputs can be individually enabled and disabled
  by a simple config flag (no need to comment out or delete large
  configuration sections)
* Use VBR for MP3 encoding
* Modified power level calculation algorithm (better sensitivity)
* Support for manual squelch setting
* Bug fixes

Version 2.0.2 (Mar 26, 2016):

* Fixed a problem with running three dongles or more, simultaneously

Version 2.0.1 (Jan 24, 2016):

* Fixed crash on output initialization

Version 2.0.0 (Dec 27, 2015):

* util/convert_cfg: can be used to convert old-style config.txt to the new format
* Syslog logging (enabled by default)
* Daemon mode
* Reworked makefiles, added install rule
* /dev/vcio is now used to access GPU on RPi; creating char_dev no longer necessary
* Startup scripts for Debian and Gentoo
* Support for auto gain setting
* Support for multiple outputs per channel
* Support for recording streams to local MP3 files
* Support for ARMv7-based platforms other than RPi (eg. Cubieboard)
* Updated documentation
* Numerous bugfixes and stability improvements

Version 1.0.0 (May 12, 2015):

* Linux x86/x86_64 support (Windows build is currently unmaintained and might not work)
* Raspberry Pi V2 support
* Bundled hello_fft code (v2.0)
* More robust interaction with Icecast servers
* Important stability fixes
