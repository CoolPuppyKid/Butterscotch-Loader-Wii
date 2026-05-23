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

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, Bytecode Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Butterscotch supports the following bytecode versions:

* Bytecode Version 13
* Bytecode Version 14
* Bytecode Version 15
* Bytecode Version 16
* Bytecode Version 17

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
