import os

from mozbuild.base import MachCommandConditions as conditions
from mozdevice.ios import IosDevice

from .host_utils import ensure_host_utils


def verify_ios_device(
    build_obj,
    install=False,
    xre=False,
    verbose=False,
    app=None,
):
    is_simulator = conditions.is_ios_simulator(build_obj)

    device_verified = False

    devices = IosDevice.all_devices(is_simulator)
    for device in devices:
        if not device.is_simulator or device.state == "Booted":
            device_verified = True
            # FIXME: This roughly mimics how `verify_android_device` works but
            # seems kinda jank - should we be copying this?
            os.environ["DEVICE_UUID"] = device.uuid
            break

    if is_simulator and not device_verified:
        # FIXME: Offer to launch a simulator here.
        print("No iOS simulator started.")
        return

    if device_verified and install:
        if not app:
            app = "org.mozilla.ios.GeckoTestBrowser"

        device = IosDevice.select_device(is_simulator)
        if app == "org.mozilla.ios.GeckoTestBrowser":
            print("Installing GeckoTestBrowser...")
            binpath = build_obj.get_binary_path("app")
            device.install(binpath)
        else:
            # FIXME: If the app is already installed, don't prompt the user here
            # to align with verify_android_device.
            input(
                "Application %s cannot be automatically installed\n"
                "Install it now, then hit Enter " % app
            )

    if device_verified and xre:
        ensure_host_utils(build_obj, verbose)

    return device_verified
