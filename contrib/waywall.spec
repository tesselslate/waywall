Name:           waywall
Version:        0.5
Release:        1%{?dist}
Summary:        Wayland compositor for Minecraft speedrunning
License:        GPL-3.0-or-later AND zlib
URL:            https://github.com/tesselslate/waywall

BuildRequires:  gcc
BuildRequires:  luajit
BuildRequires:  meson >= 1.3

BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(luajit)
BuildRequires:  pkgconfig(spng)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-cursor)
BuildRequires:  pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-composite)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  pkgconfig(xcb-xtest)
BuildRequires:  pkgconfig(xwayland)
BuildRequires:  pkgconfig(xkbcommon)

Requires:       egl-wayland
Requires:       mesa-libEGL
Requires:       mesa-libGLES
Requires:       luajit
Requires:       libspng
Requires:       libwayland-client
Requires:       libwayland-server
Requires:       libwayland-cursor
Requires:       libwayland-egl
Requires:       libxcb
Requires:       libxkbcommon
Requires:       xorg-x11-server-Xwayland

%description
waywall is a Wayland compositor designed for Minecraft speedrunning.

%prep
# Unused.

%build
%{meson}
%{meson_build}

%check
%{meson_test}

%install
%{meson_install}

install -Dm644 -t %{buildroot}%{_prefix}/local/lib64/waywall-glfw /glfw/libglfw.so*
install -Dm644 /glfw/CONTRIBUTORS.md    %{buildroot}%{_pkgdocdir}/glfw-CONTRIBUTORS.md
install -Dm644 /glfw/LICENSE.md         %{buildroot}%{_pkgdocdir}/glfw-LICENSE.md
install -Dm644 /glfw/PATCHES.md         %{buildroot}%{_pkgdocdir}/glfw-PATCHES.md

cp -r doc %{buildroot}%{_pkgdocdir}/book

%files
%license LICENSE
%doc README.md
%doc %{_pkgdocdir}/glfw-CONTRIBUTORS.md
%doc %{_pkgdocdir}/glfw-LICENSE.md
%doc %{_pkgdocdir}/glfw-PATCHES.md
%doc %{_pkgdocdir}/book/*
%{_bindir}/waywall
%{_prefix}/local/lib64/waywall-glfw/*

%post

%changelog
* Tue Oct 13 2026 tesselslate - 0.5-1
- Update for build system rewrite

* Sat Aug 16 2025 ByPaco10 <tesselslate> - 0.5-1
- Initial package release
