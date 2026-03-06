# FromVulkanToDirectX12

This repository is a fork of the original project that demonstrates a rendering pipeline using **DirectX 12 Mesh Shaders** instead of the traditional **Vertex Shader pipeline**.

The goal of this fork is to experiment with and showcase modern **DirectX 12 GPU-driven rendering features**, particularly the **Mesh Shader pipeline**

Press **F1** during execution to enable **debug rendering**, which visualizes the generated **meshlets** to help inspect how the mesh is partitioned and processed by the mesh shader pipeline.
## Required components

### CMake
The **CMake build-tools** are used to allow using any compiler and IDE.\
Install the **CMake build-tools** [here](https://cmake.org/download/).

The **CMake integration tools** for your **IDE** are also required to be able to directly open the root directory and work from there.\
\
Ex: Install Visual Studio's **CMake integration tools** from the Visual Studio Installer:\
![VS_installCMake](Doc/Pictures/VS_install_CMakeIntegration.png)

### Vulkan SDK
In order to link and compile the Vulkan example, the Vulkan SDK is required.\
/!\ When installing the Vulkan SDK, be sure to select the **Shader Toolchain Debug Symbols** to be able to link the GLSL shader compiler library (_libshaderc_combined_d.lib_) in debug:
![VkSDK_install](Doc/Pictures/VulkanSDKInstall.png)

### DirectX12
The DirectX12 libraries are **already packed with the Windows OS**, therefore, **no SDK installation** is required in order to use DirectX12.\
However, to access the **latest DirectX12 features**, the **Agility SDK** can be installed into the project to override the .dlls from the OS.\
For this project, the Agility SDK **615** is automatically downloaded and installed to make sure everyone has access to **Mesh Shaders**. The Agility SDK also provide additional includes such as:
```cpp
#include <d3dx12/d3dx12.h>
```


## Initialization

Start by **cloning** the **repository**.
```
git clone https://github.com/Maisquasar/FromVulkanToDirectX12.git
cd FromVulkanToDirectX12
```

Then init the **submodules** to fetch the **dependencies** using the command:
```
git submodule update --init --recursive
```

Now the project is ready to be open directly from the **root directory** using your IDE in "_CMake mode_".
![VS_openDir](Doc/Pictures/win_VSOpenDir.jpg)


## Authors

**Romain Bourgogne**

**Maxime "mrouffet" ROUFFET** - main developer (maximerouffet@gmail.com)