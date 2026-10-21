#
# spec file for package sonero
#
# Copyright (c) 2026 Luci (Tatsu21)
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT license.
#
# Built on OBS (home:Tatsu21/Sonero) through scmsync: OBS mirrors this git
# repository, so this file must stay at the repository root, and the sources
# arrive as the checked-out tree in %%{_sourcedir} (obs-build unpacks the
# per-directory .obscpio archives there) rather than as a release tarball.
#

Name:           sonero
Version:        0.1.4
Release:        0
Summary:        Per-application audio mixer, router and equalizer for PipeWire
License:        MIT
URL:            https://github.com/Tatsu21/sonero
BuildRequires:  cmake >= 3.21
BuildRequires:  gcc-c++
BuildRequires:  hicolor-icon-theme
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  systemd-rpm-macros
BuildRequires:  update-desktop-files
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(rnnoise)
Requires:       hicolor-icon-theme
Recommends:     pipewire
Recommends:     wireplumber
# The project used to be called LinuxSonar.
Provides:       linuxsonar = %{version}-%{release}
Obsoletes:      linuxsonar < %{version}-%{release}

%description
Sonero is a per-application audio mixer, router and equalizer for PipeWire.
Every application gets its own channel, every output device is first-class,
and each channel has its own equalizer. It also provides a microphone chain
(RNNoise noise suppression, gate, de-esser, compressor, limiter) and battery
reporting for SteelSeries headsets.

%prep
%setup -q -c -T
cp -a %{_sourcedir}/. .

%build
# The version normally comes from `git describe`; the scmsync checkout has no
# .git, so without SONERO_FALLBACK_VERSION the binary would report 0.0.0.
# spdlog stays off: packaged builds use the built-in logger.
%cmake \
    -DSONERO_FALLBACK_VERSION=%{version} \
    -DSONAR_USE_SPDLOG=OFF
%cmake_build

%install
%cmake_install
# Shipped through %%license/%%doc instead, under openSUSE's doc directory.
rm -rf %{buildroot}%{_datadir}/doc/sonero
%suse_update_desktop_file Sonero

%check
%ctest

%post
# Apply the SteelSeries HID rule without a reboot, including to a headset
# that is already plugged in.
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || :
    udevadm trigger --subsystem-match=hidraw || :
fi

%postun
if [ "$1" -eq 0 ] && command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || :
fi

%files
%license LICENSE
%doc README.md packaging/deb/copyright
%{_bindir}/Sonero
%{_datadir}/applications/Sonero.desktop
%{_datadir}/sonero/
%{_udevrulesdir}/70-sonero-steelseries.rules
%{_datadir}/icons/hicolor/16x16/apps/Sonero-tray.png
%{_datadir}/icons/hicolor/22x22/apps/Sonero-tray.png
%{_datadir}/icons/hicolor/24x24/apps/Sonero-tray.png
%{_datadir}/icons/hicolor/32x32/apps/Sonero.png
%{_datadir}/icons/hicolor/32x32/apps/Sonero-tray.png
%{_datadir}/icons/hicolor/48x48/apps/Sonero.png
%{_datadir}/icons/hicolor/48x48/apps/Sonero-tray.png
%{_datadir}/icons/hicolor/64x64/apps/Sonero.png
%{_datadir}/icons/hicolor/128x128/apps/Sonero.png
%{_datadir}/icons/hicolor/256x256/apps/Sonero.png

%changelog
