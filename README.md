<h1 align="center">🥧 Butterscotch Loader Wii 🔃</h1>

<!-- Badges, about the GitHub repository itself -->

> [!IMPORTANT]  
> This project a fork of [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)
> 
> This fork is pretty bad, Most the Wii rendering code was made with AI, And its not very serious.

**Original Butterscotch (web version):** https://butterscotch.mrpowergamerbr.com/web/

## Why is it called Wii Loader instead of Butterscotch Wii?

This project intends to be like [USB Loader GX](https://oscwii.org/library/app/usbloader_gx) but for Game Maker games.

So i wanted the name to be more similar to USB Loader GX.

## Game Compatibility

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, WAD Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Butterscotch supports the following WAD versions:

* WAD Version 8 (GameMaker: Studio 1.0.198+)
* WAD Version 9 (GameMaker: Studio 1.0.527+)
* WAD Version 10 (GameMaker: Studio 1.1.609+)
* WAD Version 11 (GameMaker: Studio 1.1.754+)
* WAD Version 12 (GameMaker: Studio 1.1.867+)
* WAD Version 13 (GameMaker: Studio 1.1.917+)
* WAD Version 14 (GameMaker: Studio 1.4.1464+)
* WAD Version 15 (GameMaker: Studio 1.4.1675+)
* WAD Version 16 (GameMaker: Studio 1.4.1767+)
* WAD Version 17 (GameMaker: Studio 2.2+)

Other modding tools, such as UndertaleModTool, calls it "bytecode version" instead of "WAD version". We decided to go with WAD version instead because there are GameMaker: Studio versions (WAD version 6 and 7) that DO NOT use bytecode altogether, so calling it "bytecode version" is not quite correct, and because that's what the YoYo Runner calls it under the hood.

Versions before GameMaker: Studio 1.0.198 (that is, pre-WAD version 8) uses raw GML code interpreted on load, so these versions would require a GML compiler to be supported in Butterscotch.

However, that doesn't mean that a game that uses a compatible version WILL run! The bytecode support is still a WIP, and Butterscotch may have quirks that the original GameMaker: Studio runner may not have.

Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode. 
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.

## Supported Platforms

* Wii (This version of butterscotch is ONLY meant for Wii)

## Original version (for if you want other platforms)
* [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)

## Building Butterscotch Loader Wii

Make sure you have ``$DEVKITPRO`` set before building, Cause like its a important library for the Wii

```bash
cmake -B build/wii -DPLATFORM=wii -DCMAKE_TOOLCHAIN_FILE=cmake/wii.cmake
cmake --build build
```

If you are using CLion, set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DPLATFORM=wii`

Then run Butterscotch in Dolphin with DATA.WIN in the /butterscotch folder on the SD Card

## Performance

Performance is pretty bad on wii, lol

## Then why not have a transpiler?

The issue with a transpiler is that, if you try transpiling the game in the "naive" way, that is, emitting VM calls like it was the original bytecode, you won't get any 
*improvement* from it, you would need to create a *good* transpiler that actually transpiles it into *good* code, and that's way harder.

Having a transpiler also have other disadvantages:

1. You lose the ability of debugging the runner at a "high level" by tracing opcodes.
2. Compilation is SLOW, transpiling Undertale in a naive way to C and building it takes 90 seconds on a modern computer, and building it to other targets is so slow that I wasn't even able to test it.

## Screenshots

# COMING SOON!
