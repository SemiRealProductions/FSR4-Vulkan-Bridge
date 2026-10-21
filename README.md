# FSR4-Vulkan-Bridge

An experimental bridge that lets supported Windows games use AMD FSR 4.1.2 FP8 through native Vulkan on Linux with Wine/Proton.
Enshrouded uses OptiScaler; Deadlock can use the bridge directly through its existing FSR3 integration.

Tested on an RX 9070 XT with RADV 26.2.5 and GE-Proton11-9.

With Enshrouded I used Optiscaler 0.9.4 as the nightly builds changed something in the load order which causes flickering.


# Linux dependencies

1. Bash and Python 3.

   Python only needs its standard library; no pip packages are required.

2. GCC and the normal Linux C development headers/libraries, including pthreads and libdl.

3. LLVM Clang and lld-link, with Windows x64 target support.

   ROCm itself is not required; the current script uses the LLVM tools from
   the ROCm installation on the test machine.

4. Vulkan development headers, including:

   - vulkan/vulkan.h
   - vulkan/vulkan_core.h
   - vulkan/vk_layer.h

5. Vulkan Utility Libraries headers, specifically:

   - vulkan/utility/vk_struct_helper.hpp

   Your distribution may package these separately from the base Vulkan headers.
   The current build.sh expects both sets of headers under /usr/include/.

6. A Windows x64 Kernel32 import library.

   The tested build uses Wine's libkernel32.a. An appropriate MinGW-w64 x64 import library is another option.

7. Check the toolchain paths before building.

   build.sh currently defaults to:

   Clang:    /usr/lib64/rocm/llvm/bin/clang
   Linker:   /usr/lib64/rocm/llvm/bin/lld-link
   Kernel32: /opt/wine-staging/lib64/wine/x86_64-windows/libkernel32.a

   If your installation differs, supply your own paths when running it:

   ```bash
   FSR_PROBE_CLANG=/path/to/clang \
   FSR_PROBE_LINKER=/path/to/lld-link \
   FSR_PROBE_KERNEL32_IMPORT=/path/to/x64/libkernel32.a \
   bash ./build.sh
   ```

8. Have 7-Zip available for extracting the downloaded files, and Steam with a working Proton setup for running the Windows versions of the games.

   Enshrouded also needs OptiScaler, as described in its section below.
   The driver and SDK downloads below supply the AMD runtime DLLs; you do not need to install the Windows AMD driver on Linux.


# Common instructions

1. Download the Windows 11 AMD Adrenalin 26.10.1 driver.

   AMD download page:
   
   https://www.amd.com/en/support/downloads/drivers.html/graphics/radeon-rx/radeon-rx-9000-series/amd-radeon-rx-9070-xt.html

3. Use 7-Zip to extract the driver executable into a new folder.

   You only need to extract the files; you do not need to install the driver.

4. Inside the extracted driver folder, open:

   whql-amd-software-adrenalin-edition-26.10.1-win11-c/Packages/Drivers/Display/WT6A_INF/B026676/

   Find amdvkffx64.dll and copy it into a new folder for your collected files.

5. Download FSR SDK 2.3 from GPUOpen:

   https://gpuopen.com/amd-fsr-sdk/

6. Inside the SDK, open:

   FidelityFX-SDK/Kits/FidelityFX/signedbin/

   Copy these two files into the same folder as amdvkffx64.dll:

   - amd_fidelityfx_loader_dx12.dll
   - amd_fidelityfx_upscaler_dx12.dll

7. Rename the copies in your collected-files folder:

   amd_fidelityfx_loader_dx12.dll -> amd_fidelityfx_loader_vk.dll

   amd_fidelityfx_upscaler_dx12.dll -> amd_fidelityfx_upscaler_vk.dll

8. Compile the custom FSR bridge by running build.sh from the custom folder. Alternatively grab the built binaries from the Release tab.

   ```bash
   bash ./build.sh
   ```

   This generates a build/ folder containing:

   - amd_fidelityfx_vk.dll
   - libnative_fsr4_layer.so

9. Copy both build outputs into your collected-files folder.

   You should now have these five files ready for the game-specific steps:

   - amd_fidelityfx_loader_vk.dll
   - amd_fidelityfx_upscaler_vk.dll
   - amdvkffx64.dll
   - amd_fidelityfx_vk.dll
   - libnative_fsr4_layer.so

   Deadlock only needs the four DLLs. Enshrouded also needs the Linux .so helper.


# Enshrouded specific instructions

1. Close Enshrouded and open its installation folder using Steam's
   Manage -> Browse local files option.

   Use the folder containing the game executable. Back up any files you will
   replace into a separate folder so you can restore the original setup.

2. Install OptiScaler alongside the game executable.
    See Optiscalers Github installation guide for how to install it:
    https://github.com/optiscaler/OptiScaler/wiki/Installation

3. Copy all five files from your collected-files folder alongside the game
   executable and OptiScaler:

   - amd_fidelityfx_loader_vk.dll
   - amd_fidelityfx_upscaler_vk.dll
   - amdvkffx64.dll
   - amd_fidelityfx_vk.dll
   - libnative_fsr4_layer.so

   Replace OptiScaler's stock amd_fidelityfx_vk.dll with the custom bridge.

4. Add the following to Enshrouded's Steam launch options:

   ```text
   LD_PRELOAD="./libnative_fsr4_layer.so${LD_PRELOAD:+:$LD_PRELOAD}" %command%
   ```

   This is the relative-path launch command tested with the helper beside the
   game executable. The helper handles its temporary Vulkan layer manifest

5. Apply the OptiScaler settings in the final section, then launch Enshrouded.

   Select DLSS Quality in the game's own settings. (You can also use Performance or Balanced)
   Press Insert to open OptiScaler and confirm FFX Upscaler shows:

   4.1.2 * native Vulkan FP8


# Deadlock specific instructions

1. Close Deadlock and open its installation folder using Steam's
   Manage -> Browse local files option.

2. Use the Windows version of Deadlock through Proton.

   In Steam's Properties -> Compatibility, select the Proton version you want to use.
   I'm using GE-Proton11-9.

3. Inside the Deadlock installation, open:

   game/bin/win64/

   This is the folder containing deadlock.exe. Back up the original
   amd_fidelityfx_vk.dll, and any other files you will replace, into a
   separate folder.

4. Copy these four DLLs from your collected-files folder beside deadlock.exe:

   - amd_fidelityfx_loader_vk.dll
   - amd_fidelityfx_upscaler_vk.dll
   - amdvkffx64.dll
   - amd_fidelityfx_vk.dll

   The custom amd_fidelityfx_vk.dll replaces the game's original Vulkan FSR DLL.
   No OptiScaler installation is needed for this direct bridge setup.

5. For the local Hero Sandbox test, use these Steam launch options:

   ```text
   %command% -insecure -vulkan
   ```

   libnative_fsr4_layer.so and LD_PRELOAD were not needed in the Deadlock configuration.

6. Launch Deadlock, select FSR3 in its upscaling settings, and enter the local Hero Sandbox through the menu.

   The game still calls this option FSR3.
   The replacement DLL routes that existing interface to AMD's FSR4.1.2 provider.

7. To check execution, close the game and rename or delete the previous native-fsr4-bridge.log beside deadlock.exe before starting a fresh run.

   After entering the sandbox, check the new log for the FP8 context, compute pipeline names starting with fsr4_model_v07_fp8_no_scale_, and:

   Native Vulkan FSR4 dispatch result=0x0000000000000000

   The successful dispatch and actual model pipeline names provide more evidence than the version label alone.


# OptiScaler recommended settings

1. These settings apply to Enshrouded's OptiScaler route. The direct Deadlock setup does not use OptiScaler or its INI settings.

2. In the game's own settings, select DLSS Quality. (You can also select Performance or Balanced, I simply used Quality for mine)

3. In OptiScaler, select FSR 3.X without "w/Dx12" and click Change Upscaler if needed. Confirm FFX Upscaler shows 4.1.2 * native Vulkan FP8.

4. Merge the following values into the matching existing sections of OptiScaler.ini:

   ```ini
   [Upscalers]
   VulkanUpscaler=fsr31

   [Inputs]
   EnableFfxInputs=false

   [FSR]
   UpscalerIndex=0
   Fsr4Update=false
   Fsr4DoNotLoadAmdxc64=true

   [Libraries]
   FfxVkPath=auto
   ```

5. If using OptiScaler Client, leave its simple Output Upscaler selector on Default and its extra FSR4 DLL Swap disabled. Keep the custom Vulkan files and the INI values above.
