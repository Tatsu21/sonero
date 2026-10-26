#
# spec file for package sonero
#
# Copyright (c) 2026 SUSE LLC
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via https://bugs.opensuse.org/
#


%define  _name  Sonero
%define  _ver   %(printf '%{version}' | awk -F\+ '{print $1}')
Name:           sonero
Version:        @SERVICE@
Release:        0
Summary:        Audio mixer, router and equalizer
License:        MIT
Group:          Productivity/Multimedia/Sound/Utilities
URL:            https://github.com/Tatsu21/sonero
Source0:        %{name}-%{version}.tar.xz
BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  hicolor-icon-theme
BuildRequires:  pkgconfig
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(spdlog)
BuildRequires:  pkgconfig(udev)
%if 0%{?suse_version}
BuildRequires:  fdupes
BuildRequires:  ninja
%else
# Fedora: %%cmake_qt6 generates for Ninja too, and %%_udevrulesdir comes from
# here rather than from the default build root.
BuildRequires:  ninja-build
BuildRequires:  systemd-rpm-macros
%endif
# rnnoise-devel exists only from Tumbleweed / Leap 16 on, and Slowroll has no
# i586 build of it; without it the microphone noise suppression is built out.
%if 0%{?suse_version} >= 1600
%ifnarch %{ix86}
BuildRequires:  pkgconfig(rnnoise)
%endif
%endif
Recommends:     pipewire
Recommends:     wireplumber
# The project used to be called LinuxSonar.
Provides:       linuxsonar = %{version}-%{release}
Obsoletes:      linuxsonar < %{version}-%{release}

%description
Per-application audio mixer, router and equalizer for Linux — every app on
its own channel, every output device first-class, built on PipeWire.

%prep
%autosetup -p1

%build
# The tarball has no .git, so the version comes from the fallback; _ver drops
# the +git suffix the service appends.
%cmake_qt6 \
    -DSONERO_FALLBACK_VERSION=%{_ver}
# Fedora defines %%cmake_qt6 but not the qt6_build/qt6_install pair; its plain
# cmake macros work on the same build directory.
%if 0%{?suse_version}
%qt6_build
%else
%cmake_build
%endif

%install
%if 0%{?suse_version}
%qt6_install
%else
%cmake_install
%endif
rm -rvf %{buildroot}%{_datadir}/doc
%if 0%{?suse_version}
%fdupes -s %{buildroot}%{_datadir}/
%endif

%check
export QT_QPA_PLATFORM=offscreen
%ctest
desktop-file-validate %{buildroot}%{_datadir}/applications/%{_name}.desktop

# Fedora's systemd package reloads and re-triggers udev itself, from a file
# trigger on the rules directory.
%if 0%{?suse_version}
%post
%udev_trigger_with_reload -s hidraw

%postun
%udev_rules_update
%endif

%files
%doc README.md
%{_bindir}/%{_name}
%{_datadir}/applications/%{_name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{_name}.??g
%{_datadir}/icons/hicolor/*/apps/%{_name}-tray.??g
%{_udevrulesdir}/70-%{name}-steelseries.rules
%{_datadir}/%{name}
%license LICENSE

%changelog
