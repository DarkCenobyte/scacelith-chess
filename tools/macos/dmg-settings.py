# dmgbuild settings of Scacelith's disk image, read by tools/macos/make-dmg.sh, which passes
# -D app=<Scacelith.app> -D background=<res/macos/dmg-background.png> -D icon=<the app's .icns>.
#
# The window shows res/macos/dmg-background.png (660 x 400 points; dmgbuild joins it with
# dmg-background@2x.png, next to it, into one HiDPI TIFF), the app on the left and a link to
# /Applications on the right, where the background's arrow and its "Drag Scacelith to
# Applications" lead. The two icon positions below are the centres of the two slots drawn on the
# background (tools/macos/dmg_background.py): move both together.
#
# dmgbuild runs this file with `defines` (the -D values) in scope.
# ruff: noqa: F821
import os.path

application = defines["app"]
appname = os.path.basename(application)

# LZFSE-compressed, read-only (macOS 10.11 and later read it; the game needs macOS 26).
format = "ULFO"
filesystem = "HFS+"

files = [application]
symlinks = {"Applications": "/Applications"}
# No hide_extensions: dmgbuild would set the extension-hidden flag in the bundle's Finder info, an
# extended attribute on Scacelith.app that the copy to /Applications keeps and that strict
# signature checks reject ("resource fork, Finder information, or similar detritus"). Finder
# shows "Scacelith" under the icon anyway: it hides the .app extension of applications.
icon = defines["icon"]  # the mounted volume's icon: the game's
background = defines["background"]

# The bounds include the window's title bar: the background's 400 points plus 28.
window_rect = ((200, 120), (660, 428))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False
include_icon_view_settings = True
include_list_view_settings = False

arrange_by = None
icon_size = 128
text_size = 13
label_pos = "bottom"
icon_locations = {
    appname: (165, 196),
    "Applications": (495, 196),
}
