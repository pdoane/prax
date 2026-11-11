# Surfaces: the native objects that devices present to. A window's surface comes from lib/app, and the backends that
# create immersive sessions' surfaces are to come.
pr_library(surface INCLUDE include)
target_link_libraries(pr_surface INTERFACE prax::core)
