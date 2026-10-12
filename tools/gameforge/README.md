# GameForge visual identity tools

The look of GameForge 3D Studio follows the PWOps / ATOM GameForge style:
deep navy surfaces, bronze and gold frames, royal blue selection and an orange
forge-fire accent.

| Script | What it writes |
| --- | --- |
| `apply_theme.py` | Re-colors Blender's built-in default theme (`release/datafiles/userdef/userdef_default_theme.c`) into the GameForge "Forge" theme. Idempotent; run it again after an upstream sync touches the default theme (`--check` fails when it needs re-running). |
| `branding/make_branding.py` | Draws the crest, the splash screen, the Linux/Windows application icons and the About dialog wordmark from code. Needs `pip install pillow numpy cairosvg fonttools`; downloads the Cinzel fonts (OFL) on first run. |

Color palette (shared by both scripts):

| Role | Color |
| --- | --- |
| Editor gaps | `#04070d` |
| Region / panel surfaces | `#0a1120` to `#172742` |
| Buttons | `#203453` |
| Widget frames | bronze `#76582e` |
| Active frames, panel titles | gold `#d9ad55` / `#f2c869` |
| Selection | royal blue `#2a6fd6` |
| Progress / fire accent | `#e08a2a` |
| Text | parchment `#e9e0cc` |

Existing users keep their saved theme; pick **Preferences > Themes > GameForge Dark**
(or *Reset to Default*) to switch.
