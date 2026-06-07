{
  lib,
  hyprland,
  hyprlandPlugins,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprwinwrap";
  version = "1.1";
  src = ./.;

  inherit (hyprland) nativeBuildInputs;

  meta = with lib; {
    homepage = "https://github.com/gen3vra/hyprwinwrap";
    description = "Display any window as a background wallpaper";
    license = licenses.bsd3;
    platforms = platforms.linux;
  };
}
