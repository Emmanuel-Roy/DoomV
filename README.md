# DoomV - RV32IM Emulator Capable of Running Doom

# About.

#### Since my Gameboy emulator has been completed, I've taken more advanced coursework in hardware design and computer architecture, to the point where I have one more undergraduate class left. I've gotten internships and had cool things to work on, but i've been itching for another big project to sink my teeth into. I have a month left before I start doing a Co-op, so I figured I'd work on something to keep my programming skills sharp.

# Goal

### Boot bare-metal DOOM. If it works, nothing else needs to be implemented. 

##### Maybe add JIT

# GUI and Overall System Archtecture.

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/92adbcb6-cfe2-464e-8a09-8b817151a260" />

# CPU Core Explanation

##### This RISC-V is based on the RISC-V reference sheet from this GitHub:  
[https://github.com/jameslzhu/riscv-card](https://github.com/jameslzhu/riscv-card)

#### Note that I'm not doing the Floating Point extension for now, perhaps I will if performance is bad. (Which I suspect it will be due to the nature of the vibe-coded GUI.

#### Aside from that, to put things simply, we are back to using the primary switch case architecture. I considered using something like an array of function pointers, but after some research, I found out that industry tools like QEMU stick to this switch approach, and that it's better to optimize cases together rather than swapping to the function pointer approach. My research led me to the idea that function pointers can cause issues with branch prediction, so I think this switch approach is more performant (even if its kinda ugly).

#### While Atomics may seem useless, I'm guessing the game will depend on them or won't build otherwise. In practice, all instructions here are atomic. 

#### We need a couple more packages than IM, source one used a couple more than I think i'll need. I'll be implementing Zicsr, CLINT, UART, and the Framebuffer as well. Zifencei won't be fully implemented, I plan to treat it like a NOP unless we need to add it for some reason. Many more may be added.

# Sources:
[https://github.com/jameslzhu/riscv-card](https://git.knazarov.com/knazarov/rve/) - I originally was inspired by his RISC-V core, and I plan to use his port for this project, or another with slight modifications. I didn't read any code from his repo, but I used it to help determine what extensions I would need to implement to get this project going. 
