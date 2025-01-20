# mesa-21.3.5 с поддержкой PowerVR

## Зависимости
список зависимостей, обратить внимание на версию meson и llvm. Их можно взять из репозитория Debian 12. Зависимости:

* python 3
* пайтон модуль mako
* clang
* pkg-config
* cmake
* meson=1.0.1-5
* libdrm2 и libdrm-dev (в случае проблем с libdrm использовать версию [2.4.102](https://gitlab.freedesktop.org/mesa/drm/-/tree/libdrm-2.4.102))
* llvm-14, удалить llvm-19 на время сборки, иначе meson всегда будет использовать его  
* libelf-dev
* bison
* flex
* libwayland-dev
* wayland-protocols
* libwayland-egl-backend-dev
* libxfixes-dev
* libxcb-glx0-dev
* libxcb-shm0-dev
* libxxf86vm-dev
* libx11-xcb-dev
* libxcb-dri2-0-dev
* libxcb-dri3-dev
* libxcb-present-dev
* libxshmfence-dev
* x11-xserver-utils
* libxrandr-dev

## Сборка
Использовать clang для компиляции, стандарт С++17, LLVM компоновать статически: ```CC=clang CXX=clang++ meson setup build/ -Dshared-llvm=disabled -Dc_std=c99 -Dcpp_std=c++17```

Если во время сборки появятся ошибки, связанные с valgrind, явно отключить его, добавив опцию ```-Dvalgrind=disabled```. Это баг в meson-файле mesa-21.3.5, насколько я понял

После конфигурации запустить ```ninja -C build``` для сборки

Для установки выполнить ```ninja -C build install``` с правами суперпользователя

## Дополнительная информация
Исходники драйвера для PowerVR хранятся в src/mesa/drivers/dri/pvr. Набраны из mesa-17 и [патча](https://chromium.googlesource.com/chromiumos/overlays/chromiumos-overlay/+/master/media-libs/arc-mesa-img/files/0001-dri-pvr-Introduce-PowerVR-DRI-driver.patch) для ChromiumOS