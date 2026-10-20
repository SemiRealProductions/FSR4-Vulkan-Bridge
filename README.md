# FSR4-Vulkan-Bridge

FSR4 Vulkan setup with OptiScaler
===============================

1. Download the Windows 11 AMD Adrenalin 26.10.1 driver.

   AMD download page:
   https://www.amd.com/en/support/downloads/drivers.html/graphics/radeon-rx/radeon-rx-9000-series/amd-radeon-rx-9070-xt.html

2. Use 7-Zip to extract the driver executable into a new folder.

   You only need to extract the files; you do not need to install the driver.

3. Inside the extracted driver folder, open:

   whql-amd-software-adrenalin-edition-26.10.1-win11-c/
     Packages/Drivers/Display/WT6A_INF/B026676/

   Find amdvkffx64.dll and copy it into a new folder for your collected files.

4. Download FSR SDK 2.3 from GPUOpen:

   https://gpuopen.com/amd-fsr-sdk/

5. Inside the SDK, open:

   FidelityFX-SDK/Kits/FidelityFX/signedbin/

   Copy these two files into the same folder as amdvkffx64.dll:

   - amd_fidelityfx_loader_dx12.dll
   - amd_fidelityfx_upscaler_dx12.dll

6. Rename the copies in your collected-files folder:

   amd_fidelityfx_loader_dx12.dll -> amd_fidelityfx_loader_vk.dll

   amd_fidelityfx_upscaler_dx12.dll -> amd_fidelityfx_upscaler_vk.dll

7. Compile the custom FSR bridge by running build.sh from the custom folder.

   This generates a build/ folder containing:

   - amd_fidelityfx_vk.dll
   - libnative_fsr4_layer.so

8. Install OptiScaler into your chosen game's folder, alongside the executable.

9. Gather the files to copy:

   From your collected-files folder:
   - amd_fidelityfx_loader_vk.dll
   - amd_fidelityfx_upscaler_vk.dll
   - amdvkffx64.dll

   From the custom bridge's build/ folder:
   - amd_fidelityfx_vk.dll
   - libnative_fsr4_layer.so

10. Copy all five files into the game's folder, alongside the executable and OptiScaler.

11. Add the following to the game's Steam launch options:

    ```LD_PRELOAD="./libnative_fsr4_layer.so${LD_PRELOAD:+:$LD_PRELOAD}" %command%```
