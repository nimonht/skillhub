Name:           skillhub
Version:        1.0.1
Release:        1%{?dist}
Summary:        One skills folder for every coding agent
License:        MIT AND OFL-1.1 AND CC0-1.0
URL:            https://github.com/nimonht/skillhub
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  git-core
BuildRequires:  libuv-devel
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       git-core
Requires:       qt6-qtdeclarative%{?_isa}

%description
Skill Hub finds the AI coding agents installed on the machine (Claude Code,
Codex, Cursor, OpenCode, Gemini CLI, Antigravity, Zed and many more), keeps one
shared folder of agent skills for all of them, installs skills from GitHub,
keeps them up to date and moves the whole set to another computer with one file.

A small command line written in C (skillhub) and a native Qt 6 window
(skillhub-gui), which `skillhub` starts when run without a command.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%check
python3 -I tests/catalog-check.py
python3 -I tests/portable_smoke.py %{_vpath_builddir}/skillhub
desktop-file-validate %{buildroot}%{_datadir}/applications/skillhub.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.nimonht.skillhub.metainfo.xml

%files
%license LICENSE NOTICE.md data/LICENSE-vercel-labs-skills data/LICENSE-lobe-icons data/LICENSE-simple-icons fonts/OFL.txt
%doc README.md SECURITY.md
%{_bindir}/skillhub
%{_bindir}/skillhub-gui
%{_datadir}/applications/skillhub.desktop
%{_metainfodir}/io.github.nimonht.skillhub.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/skillhub.png
%{_datadir}/icons/hicolor/scalable/apps/skillhub.svg

%changelog
* Sat Oct 24 2026 nimonht <181256175+nimonht@users.noreply.github.com> - 1.0.1-1
- Windows junction links, packaging fixes

* Sat Oct 24 2026 nimonht <181256175+nimonht@users.noreply.github.com> - 1.0.0-1
- First release
