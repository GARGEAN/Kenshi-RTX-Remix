# PHASMOPHOBIA BUILD: Minimal nv-private/meson.build
# Created to allow build to proceed without full nv-private content

# Only build hdremix if the directory has the required files
if fs.is_dir('hdremix') and fs.exists('hdremix/meson.build')
  subdir('hdremix')
endif
