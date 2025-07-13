{pkgs ? import <nixpkgs> {}, lib ? lib }:
pkgs.mkShell {

  buildInputs = with pkgs; [
    wayland
    libxkbcommon
    cairo
    wayland-protocols
    libglvnd
    luajit
    libspng
    xorg.libxcb
    xorg.xcbutil
    xorg.xcbutilwm
    xorg.xcbutilimage
    xorg.xcbutilkeysyms
    xorg.xcbutilcursor
    xorg.xcbutilrenderutil
    xorg.xcbutilerrors
    xorg.libXcomposite
    xorg.libXres
    xorg.libXtst
    xwayland
    wayland-scanner
  ];

  nativeBuildInputs = with pkgs; [
    pkg-config
    cmake
    ninja
    meson
  ];
}