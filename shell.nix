{
  pkgs ? import <nixpkgs> { },
}:

pkgs.mkShell {
  packages = with pkgs; [
    # C/C++
    gcc
    clang
    clang-tools
    gdb

    # Build system
    cmake
    ninja
    pkg-config

    # GTK
    gtk4
    gtkmm4
    gtk4-layer-shell

    # Wayland
    wayland
    wayland-protocols
    wayland-utils

    # Graphics / UI
    cairo
    pango
    gdk-pixbuf
    graphene

    # Development
    git
    nixfmt

    pipewire
    pkgs.nlohmann_json
  ];

  shellHook = ''
    export CC=gcc
    export CXX=g++

    echo "Wavel development environment"
    echo "C++20 | GTK4 | GTKmm4 | Wayland"
  '';
}
