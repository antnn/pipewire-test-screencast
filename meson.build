project('dbusdemo', 'c')

gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pipewire_dep = dependency('libpipewire-0.3')
sdl2_dep = dependency('sdl2')


executable('dbusdemo', 'main.c', dependencies: [gio_dep, gio_unix_dep, pipewire_dep,sdl2_dep])
