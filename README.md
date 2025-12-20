# DoomV - RV32IMAC Emulator Capable of Running Doom

# About.

#### Since my Gameboy emulator has been completed, I've taken more advanced coursework in hardware design and computer architecture, to the point where I have one more undergraduate class left. I've gotten internships and had cool things to work on, but i've been itching for another big project to sink my teeth into. I have a month left before I start doing a Co-op, so I figured I'd work on something to keep my programming skills sharp.

# GUI and Overall System Archtecture.

### DISCLAIMER: AI was used to assist me during this part of development. However, it will NOT be used when developing or debugging the CPU Core. The initial commit and the underlying system code is pretty vibe-coded.

#### The reasoning for this is that while I think AI is an excellent tool at making sure this project actually gets done, i'd rather work on my underlying skills for when things eventually break in my IRL work.

###### Unless I get really desperate :/ (I don't have any seniors to ask for help), if it comes to that I'll mark what was vibe coded with a comment.

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/92adbcb6-cfe2-464e-8a09-8b817151a260" />

# CPU Core Explanation

##### This RISC-V is based on the RISC-V reference sheet from this GitHub:  
[https://github.com/jameslzhu/riscv-card](https://github.com/jameslzhu/riscv-card)

#### Note that I'm not doing the Floating Point extension for now, perhaps I will if performance is bad. (Which I suspect it will be due to the nature of the vibe-coded GUI.

#### Aside from that, to put things simply, we are back to using the primary switch case architecture. I considered using something like an array of function pointers, but after some research, I found out that industry tools like QEMU stick to this switch approach, and that it's better to optimize cases together rather than swapping to the function pointer approach. My research led me to the idea that function pointers can cause issues with branch prediction, so I think this switch approach is more performant (even if its kinda ugly).

