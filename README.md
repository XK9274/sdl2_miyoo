# Warning:
### The main branch of this fork is out of date. The branches are out of date but the `vanilla` branch has most of the NDS specific logic removed and can be used at runtime for various apps.
### If your app uses TARGET textures, you will likely find they do NOT work for your application. 
### You also cannot use SDL_SetRenderTarget as it's not implemented.
### To use these libs, use a STREAMING texture, blit everything to a surface and then update your final texture & rendercopy/present, an example of which is here:

```c
void RenderManagerSDL::refresh()
{
	SDL_RenderClear(mRenderer);
	SDL_UpdateTexture(mRenderStreaming, NULL, mMiyooSurface->pixels, mMiyooSurface->pitch);
	SDL_RenderCopy(mRenderer, mRenderStreaming, NULL, NULL);
	SDL_RenderPresent(mRenderer);
}
```



# SDL2 Library with Virtual GPU Support for Miyoo Mini (Plus) and TRIMUI SMART
 - [Miyoo Mini (Plus)](#miyoo-mini-plus)
   - Introduction
   - Build from Scratch
     - How to prepare the build environment (Docker)
     - How to build all libraries
     - How to pack the release build
     - How to delete the build environment (Docker)
 - [TRIMUI SMART](#trimui-smart)
   - Introduction
   - Build from Scratch
     - How to prepare the build environment (Docker)
     - How to build all libraries
     - How to pack the release build
     - How to delete the build environment (Docker)

&nbsp;

## Miyoo Mini (Plus)
![image](images/mmiyoo/mm.jpg) ![image](images/mmiyoo/mmp.jpg)  

### Introduction
TBD

&nbsp;

### Build from Scratch
#### How to prepare the build environment (Docker)
```
$ sudo docker build -t mmiyoo .
```

#### How to build all libraries (SDL2 and virtual GPU)
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make config
$ make
```

#### How to build the SDL2 library only
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make sdl2
```

#### How to build the virtual GPU (swiftshader) library only
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make gpu
```

#### How to build the example
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make example
```

#### How to delete the build environment (Docker)
```
$ sudo docker image rm mmiyoo
```

&nbsp;

## TRIMUI SMART
![image](images/trimui/trimui.jpg)  

### Introduction
TBD

&nbsp;

### Build from Scratch
#### How to prepare the build environment (Docker)
```
$ sudo docker build -t mmiyoo .
```

#### How to build all libraries (SDL2 and virtual GPU)
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make config MOD=trimui
$ make
```

#### How to build the SDL2 library only
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make sdl2
```

#### How to build the virtual GPU (swiftshader) library only
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make gpu
```

#### How to build the example
```
$ sudo docker run -it --rm -v $(pwd):/nds_miyoo mmiyoo /bin/bash
$ make example MOD=trimui
```

#### How to delete the build environment (Docker)
```
$ sudo docker image rm mmiyoo
```
