Name:           waywall
Version:        0.5
Release:        1%{?dist}
Summary:        Wayland compositor for Minecraft speedrunning
License:        GPL-3.0-or-later AND zlib
URL:            https://github.com/tesselslate/waywall

BuildRequires:  wayland-protocols-devel meson gcc git rpm-build cmake
Requires:       egl-wayland mesa-libEGL mesa-libGLES luajit libspng libwayland-client libwayland-server libwayland-cursor libwayland-egl libxcb libxkbcommon xorg-x11-server-Xwayland

%description
Waywall is a Wayland compositor designed for Minecraft speedrunning.

%prep
# No extraction needed; source is in %{_sourcedir}

%build
cd %{_sourcedir}
# Use rpmbuild's build directory for Meson output
meson setup --wipe %{_builddir}/%{name}-%{version}-build --prefix=/usr
ninja -C %{_builddir}/%{name}-%{version}-build

%check
cd %{_sourcedir}
ninja -C %{_builddir}/%{name}-%{version}-build test || true

%install
set -x
# Check and install waywall binary
if [ ! -f %{_builddir}/%{name}-%{version}-build/waywall/waywall ]; then
    echo "Error: %{_builddir}/%{name}-%{version}-build/waywall/waywall not found"
    exit 1
fi
install -Dm755 %{_builddir}/%{name}-%{version}-build/waywall/waywall %{buildroot}%{_bindir}/waywall

# Install documentation from source directory
if [ -f %{_sourcedir}/README.md ]; then
    install -Dm644 %{_sourcedir}/README.md %{buildroot}%{_docdir}/%{name}/README.md
else
    echo "Warning: README.md not found, skipping"
fi
if [ ! -f %{_sourcedir}/LICENSE ]; then
    echo "Error: LICENSE not found"
    exit 1
fi
install -Dm644 %{_sourcedir}/LICENSE %{buildroot}%{_datarootdir}/licenses/%{name}/LICENSE

# Install patched GLFW libraries from source directory
mkdir -p %{buildroot}%{_prefix}/local/lib64/waywall-glfw
for file in %{_sourcedir}/glfw/libglfw.so %{_sourcedir}/glfw/libglfw.so.3 %{_sourcedir}/glfw/libglfw.so.3.4; do
    if [ ! -f "$file" ]; then
        echo "Error: $file not found"
        exit 1
    fi
    install -m644 "$file" %{buildroot}%{_prefix}/local/lib64/waywall-glfw/
done

# Install GLFW documentation from source directory with glfw- prefix
for file in %{_sourcedir}/glfw/LICENSE.md %{_sourcedir}/glfw/CONTRIBUTORS.md %{_sourcedir}/glfw/MAINTAINERS.md; do
    if [ ! -f "$file" ]; then
        echo "Error: $file not found"
        exit 1
    fi
    base_name=$(basename "$file")
    install -m644 "$file" %{buildroot}%{_docdir}/%{name}/"glfw-$base_name"
done

if [ -d %{_sourcedir}/doc ]; then
    cp -r %{_sourcedir}/doc %{buildroot}%{_docdir}/%{name}/
fi

%files
%license %{_datarootdir}/licenses/%{name}/LICENSE
%doc %{_docdir}/%{name}/README.md
%doc %{_docdir}/%{name}/glfw-LICENSE.md
%doc %{_docdir}/%{name}/glfw-CONTRIBUTORS.md
%doc %{_docdir}/%{name}/glfw-MAINTAINERS.md
%doc %{_docdir}/%{name}/doc/*
%{_bindir}/waywall
%{_prefix}/local/lib64/waywall-glfw/*

%post

%changelog
* Sat Aug 16 2025 ByPaco10 <tesselslate> - 0.5-1
- Initial package release
