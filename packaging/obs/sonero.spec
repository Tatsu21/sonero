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
BuildRequires:  fdupes
BuildRequires:  gcc-c++
BuildRequires:  hicolor-icon-theme
BuildRequires:  pkgconfig
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(rnnoise)
BuildRequires:  pkgconfig(spdlog)
BuildRequires:  pkgconfig(udev)
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
%qt6_build

%install
%qt6_install
rm -rvf %{buildroot}%{_datadir}/doc
%fdupes -s %{buildroot}%{_datadir}/

%check
export QT_QPA_PLATFORM=offscreen
%ctest
desktop-file-validate %{buildroot}%{_datadir}/applications/%{_name}.desktop

%post
%udev_trigger_with_reload -s hidraw

%postun
%udev_rules_update

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
