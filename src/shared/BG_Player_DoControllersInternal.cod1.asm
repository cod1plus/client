0x01a389  push     ebp                            
0x01a38a  mov      ebp, esp                       
0x01a38c  push     esi                            
0x01a38d  push     ebx                            
0x01a38e  sub      esp, 0xe0                      
0x01a394  call     0x1b47c                        
0x01a399  add      ebx, 0x6e9ff                   
0x01a39f  mov      eax, dword ptr [ebp + 0xc]     
0x01a3a2  mov      edx, 0xffffc000                
0x01a3a7  movzx    eax, word ptr [eax + 8]        
0x01a3ab  and      eax, edx                       
0x01a3ad  test     ax, ax                         
0x01a3b0  je       0x1a3d2                        
0x01a3b2  mov      dword ptr [esp + 8], 0x60      
0x01a3ba  mov      dword ptr [esp + 4], 0         
0x01a3c2  mov      eax, dword ptr [ebp + 0x18]    
0x01a3c5  mov      dword ptr [esp], eax           
0x01a3c8  call     0x15330                        
0x01a3cd  jmp      0x1ab78                        
0x01a3d2  mov      eax, dword ptr [ebp + 0xc]     
0x01a3d5  mov      eax, dword ptr [eax + 0x90]    
0x01a3db  mov      dword ptr [ebp - 0xac], eax    
0x01a3e1  mov      eax, dword ptr [ebx - 0x1504c]   ; .rodata 0x073d4c = 0
0x01a3e7  mov      dword ptr [ebp - 0x60], eax    
0x01a3ea  mov      dword ptr [ebp - 0x64], eax    
0x01a3ed  mov      dword ptr [ebp - 0x68], eax    
0x01a3f0  mov      eax, dword ptr [ebx - 0x1504c]   ; .rodata 0x073d4c = 0
0x01a3f6  mov      dword ptr [ebp - 0x90], eax    
0x01a3fc  mov      dword ptr [ebp - 0x94], eax    
0x01a402  mov      dword ptr [ebp - 0x98], eax    
0x01a408  mov      eax, dword ptr [ebp + 0x14]    
0x01a40b  mov      eax, dword ptr [eax + 0x3e8]   
0x01a411  mov      dword ptr [ebp - 0xa8], eax    
0x01a417  mov      eax, dword ptr [ebp + 0x14]    
0x01a41a  mov      eax, dword ptr [eax + 0x3ec]   
0x01a420  mov      dword ptr [ebp - 0xa4], eax    
0x01a426  mov      eax, dword ptr [ebp + 0x14]    
0x01a429  mov      eax, dword ptr [eax + 0x3f0]   
0x01a42f  mov      dword ptr [ebp - 0xa0], eax    
0x01a435  mov      eax, dword ptr [ebp + 0x14]    
0x01a438  mov      eax, dword ptr [eax + 0x380]   
0x01a43e  mov      dword ptr [ebp - 0x64], eax    
0x01a441  mov      eax, dword ptr [ebp + 0x14]    
0x01a444  mov      eax, dword ptr [eax + 0x3b0]   
0x01a44a  mov      dword ptr [ebp - 0x94], eax    
0x01a450  mov      dword ptr [esp + 8], 0         
0x01a458  mov      dword ptr [esp + 4], 3         
0x01a460  mov      eax, dword ptr [ebp + 0x14]    
0x01a463  mov      dword ptr [esp], eax           
0x01a466  call     0x188a4                        
0x01a46b  and      eax, 0x30000                   
0x01a470  test     eax, eax                       
0x01a472  jne      0x1a4e5                        
0x01a474  mov      eax, dword ptr [ebp + 0x14]    
0x01a477  mov      eax, dword ptr [eax + 0x3b8]   
0x01a47d  mov      dword ptr [ebp - 0x98], eax    
0x01a483  mov      eax, dword ptr [ebp + 0xc]     
0x01a486  mov      eax, dword ptr [eax + 8]       
0x01a489  and      eax, 0x40                      
0x01a48c  test     eax, eax                       
0x01a48e  je       0x1a4e5                        
0x01a490  mov      eax, dword ptr [ebp - 0x98]    
0x01a496  mov      dword ptr [esp], eax           
0x01a499  call     0x15a20                        
0x01a49e  fstp     dword ptr [ebp - 0x98]         
0x01a4a4  fld      dword ptr [ebp - 0x98]         
0x01a4aa  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a4b0  fxch     st(1)                          
0x01a4b2  fucompp                                 
0x01a4b4  fnstsw   ax                             
0x01a4b6  sahf                                    
0x01a4b7  ja       0x1a4bb                        
0x01a4b9  jmp      0x1a4d1                        
0x01a4bb  fld      dword ptr [ebp - 0x98]         
0x01a4c1  fld      dword ptr [ebx - 0x15048]        ; .rodata 0x073d50 = 0.5
0x01a4c7  fmulp    st(1)                          
0x01a4c9  fstp     dword ptr [ebp - 0x98]         
0x01a4cf  jmp      0x1a4e5                        
0x01a4d1  fld      dword ptr [ebp - 0x98]         
0x01a4d7  fld      dword ptr [ebx - 0x15044]        ; .rodata 0x073d54 = 0.25
0x01a4dd  fmulp    st(1)                          
0x01a4df  fstp     dword ptr [ebp - 0x98]         
0x01a4e5  lea      eax, [ebp - 0xa8]              
0x01a4eb  mov      dword ptr [esp + 8], eax       
0x01a4ef  lea      eax, [ebp - 0x98]              
0x01a4f5  mov      dword ptr [esp + 4], eax       
0x01a4f9  lea      eax, [ebp - 0xa8]              
0x01a4ff  mov      dword ptr [esp], eax           
0x01a502  call     0x14a50                        
0x01a507  lea      eax, [ebp - 0x98]              
0x01a50d  mov      dword ptr [esp + 8], eax       
0x01a511  lea      eax, [ebp - 0x68]              
0x01a514  mov      dword ptr [esp + 4], eax       
0x01a518  lea      eax, [ebp - 0x98]              
0x01a51e  mov      dword ptr [esp], eax           
0x01a521  call     0x14a50                        
0x01a526  mov      dword ptr [ebp - 0x78], 0      
0x01a52d  mov      dword ptr [ebp - 0x74], 0      
0x01a534  mov      eax, dword ptr [ebp + 0xc]     
0x01a537  mov      eax, dword ptr [eax + 0xe4]    
0x01a53d  mov      dword ptr [ebp - 0x70], eax    
0x01a540  mov      eax, dword ptr [ebp + 0x14]    
0x01a543  mov      eax, dword ptr [eax + 0x3e4]   
0x01a549  mov      dword ptr [esp], eax           
0x01a54c  call     0x149d0                        
0x01a551  fstp     dword ptr [ebp - 0x7c]         
0x01a554  fld      dword ptr [ebp - 0x7c]         
0x01a557  fmul     dword ptr [ebx - 0x15068]        ; .rodata 0x073d30 = 50
0x01a55d  fld      dword ptr [ebx - 0x15040]        ; .rodata 0x073d58 = 0.925
0x01a563  fmulp    st(1)                          
0x01a565  fstp     dword ptr [ebp - 0x90]         
0x01a56b  fld      dword ptr [ebp - 0x7c]         
0x01a56e  fmul     dword ptr [ebx - 0x15068]        ; .rodata 0x073d30 = 50
0x01a574  fld      dword ptr [ebx - 0x15040]        ; .rodata 0x073d58 = 0.925
0x01a57a  fmulp    st(1)                          
0x01a57c  fstp     dword ptr [ebp - 0xa0]         
0x01a582  mov      eax, dword ptr [ebp - 0x7c]    
0x01a585  mov      dword ptr [ebp - 0xcc], eax    
0x01a58b  fld      dword ptr [ebp - 0xcc]         
0x01a591  fchs                                    
0x01a593  fld      dword ptr [ebx - 0x1503c]        ; .rodata 0x073d5c = 2.5
0x01a599  fmulp    st(1)                          
0x01a59b  fld      dword ptr [ebp - 0x74]         
0x01a59e  faddp    st(1)                          
0x01a5a0  fstp     dword ptr [ebp - 0x74]         
0x01a5a3  mov      eax, dword ptr [ebp + 0xc]     
0x01a5a6  mov      eax, dword ptr [eax + 8]       
0x01a5a9  and      eax, 1                         
0x01a5ac  test     eax, eax                       
0x01a5ae  jne      0x1a5cb                        
0x01a5b0  mov      eax, dword ptr [ebp + 0x14]    
0x01a5b3  mov      eax, dword ptr [eax + 0x3ec]   
0x01a5b9  mov      dword ptr [esp + 4], eax       
0x01a5bd  mov      eax, dword ptr [ebp - 0x64]    
0x01a5c0  mov      dword ptr [esp], eax           
0x01a5c3  call     0x14450                        
0x01a5c8  fstp     dword ptr [ebp - 0x64]         
0x01a5cb  mov      eax, dword ptr [ebp + 0xc]     
0x01a5ce  mov      eax, dword ptr [eax + 8]       
0x01a5d1  and      eax, 0x40                      
0x01a5d4  test     eax, eax                       
0x01a5d6  je       0x1a7c0                        
0x01a5dc  fld      dword ptr [ebp - 0x7c]         
0x01a5df  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a5e5  fxch     st(1)                          
0x01a5e7  fucompp                                 
0x01a5e9  fnstsw   ax                             
0x01a5eb  sahf                                    
0x01a5ec  jne      0x1a5f2                        
0x01a5ee  jp       0x1a5f2                        
0x01a5f0  jmp      0x1a606                        
0x01a5f2  fld      dword ptr [ebp - 0xa0]         
0x01a5f8  fld      dword ptr [ebx - 0x15048]        ; .rodata 0x073d50 = 0.5
0x01a5fe  fmulp    st(1)                          
0x01a600  fstp     dword ptr [ebp - 0xa0]         
0x01a606  mov      eax, dword ptr [ebp + 0xc]     
0x01a609  fld      dword ptr [ebp - 0x68]         
0x01a60c  fadd     dword ptr [eax + 0xe8]         
0x01a612  fstp     dword ptr [ebp - 0x68]         
0x01a615  lea      eax, [ebp - 0xb4]              
0x01a61b  mov      dword ptr [esp + 8], eax       
0x01a61f  lea      eax, [ebp - 0xb0]              
0x01a625  mov      dword ptr [esp + 4], eax       
0x01a629  fld      dword ptr [ebp - 0x94]         
0x01a62f  fld      qword ptr [ebx - 0x15038]        ; .rodata 0x073d60
0x01a635  fmulp    st(1)                          
0x01a637  fld      qword ptr [ebx - 0x15030]        ; .rodata 0x073d68 = 0
0x01a63d  fdivp    st(1)                          
0x01a63f  fstp     dword ptr [esp]                
0x01a642  call     0x1b464                        
0x01a647  fld      dword ptr [ebx - 0x15028]        ; .rodata 0x073d70 = 1
0x01a64d  fsub     dword ptr [ebp - 0xb4]         
0x01a653  fld      dword ptr [ebx - 0x15024]        ; .rodata 0x073d74 = -24
0x01a659  fmulp    st(1)                          
0x01a65b  fld      dword ptr [ebp - 0x78]         
0x01a65e  faddp    st(1)                          
0x01a660  fstp     dword ptr [ebp - 0x78]         
0x01a663  fld      dword ptr [ebp - 0xb0]         
0x01a669  fld      dword ptr [ebx - 0x15020]        ; .rodata 0x073d78 = -12
0x01a66f  fmulp    st(1)                          
0x01a671  fld      dword ptr [ebp - 0x74]         
0x01a674  faddp    st(1)                          
0x01a676  fstp     dword ptr [ebp - 0x74]         
0x01a679  fld      dword ptr [ebp - 0x7c]         
0x01a67c  fmul     dword ptr [ebp - 0xb0]         
0x01a682  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a688  fxch     st(1)                          
0x01a68a  fucompp                                 
0x01a68c  fnstsw   ax                             
0x01a68e  sahf                                    
0x01a68f  ja       0x1a693                        
0x01a691  jmp      0x1a6c2                        
0x01a693  mov      eax, dword ptr [ebp - 0x7c]    
0x01a696  mov      dword ptr [ebp - 0xcc], eax    
0x01a69c  fld      dword ptr [ebp - 0xcc]         
0x01a6a2  fchs                                    
0x01a6a4  fld      dword ptr [ebx - 0x15028]        ; .rodata 0x073d70 = 1
0x01a6aa  fsub     dword ptr [ebp - 0xb4]         
0x01a6b0  fmulp    st(1)                          
0x01a6b2  fld      dword ptr [ebx - 0x1501c]        ; .rodata 0x073d7c = 16
0x01a6b8  fmulp    st(1)                          
0x01a6ba  fld      dword ptr [ebp - 0x74]         
0x01a6bd  faddp    st(1)                          
0x01a6bf  fstp     dword ptr [ebp - 0x74]         
0x01a6c2  mov      dword ptr [ebp - 0x58], 0      
0x01a6c9  fld      dword ptr [ebp - 0x90]         
0x01a6cf  fld      dword ptr [ebx - 0x15018]        ; .rodata 0x073d80 = -1.2
0x01a6d5  fmulp    st(1)                          
0x01a6d7  fstp     dword ptr [ebp - 0x54]         
0x01a6da  fld      dword ptr [ebp - 0x90]         
0x01a6e0  fld      dword ptr [ebx - 0x15014]        ; .rodata 0x073d84 = 0.3
0x01a6e6  fmulp    st(1)                          
0x01a6e8  fstp     dword ptr [ebp - 0x50]         
0x01a6eb  mov      eax, dword ptr [ebp + 0xc]     
0x01a6ee  fld      dword ptr [eax + 0xe8]         
0x01a6f4  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a6fa  fxch     st(1)                          
0x01a6fc  fucompp                                 
0x01a6fe  fnstsw   ax                             
0x01a700  sahf                                    
0x01a701  jne      0x1a721                        
0x01a703  jp       0x1a721                        
0x01a705  mov      eax, dword ptr [ebp + 0xc]     
0x01a708  fld      dword ptr [eax + 0xec]         
0x01a70e  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a714  fxch     st(1)                          
0x01a716  fucompp                                 
0x01a718  fnstsw   ax                             
0x01a71a  sahf                                    
0x01a71b  jne      0x1a721                        
0x01a71d  jp       0x1a721                        
0x01a71f  jmp      0x1a747                        
0x01a721  mov      eax, dword ptr [ebp + 0xc]     
0x01a724  mov      eax, dword ptr [eax + 0xec]    
0x01a72a  mov      dword ptr [esp + 4], eax       
0x01a72e  mov      eax, dword ptr [ebp + 0xc]     
0x01a731  mov      eax, dword ptr [eax + 0xe8]    
0x01a737  mov      dword ptr [esp], eax           
0x01a73a  call     0x14450                        
0x01a73f  fld      dword ptr [ebp - 0x58]         
0x01a742  faddp    st(1)                          
0x01a744  fstp     dword ptr [ebp - 0x58]         
0x01a747  mov      dword ptr [ebp - 0x4c], 0      
0x01a74e  fld      dword ptr [ebp - 0x94]         
0x01a754  fld      dword ptr [ebx - 0x15010]        ; .rodata 0x073d88 = 0.1
0x01a75a  fmulp    st(1)                          
0x01a75c  fld      dword ptr [ebp - 0x90]         
0x01a762  fld      dword ptr [ebx - 0x1500c]        ; .rodata 0x073d8c = 0.2
0x01a768  fmulp    st(1)                          
0x01a76a  fsubp    st(1)                          
0x01a76c  fstp     dword ptr [ebp - 0x48]         
0x01a76f  fld      dword ptr [ebp - 0x90]         
0x01a775  fld      dword ptr [ebx - 0x1500c]        ; .rodata 0x073d8c = 0.2
0x01a77b  fmulp    st(1)                          
0x01a77d  fstp     dword ptr [ebp - 0x44]         
0x01a780  mov      eax, dword ptr [ebp - 0x98]    
0x01a786  mov      dword ptr [ebp - 0x40], eax    
0x01a789  fld      dword ptr [ebp - 0x94]         
0x01a78f  fld      dword ptr [ebx - 0x15008]        ; .rodata 0x073d90 = 0.8
0x01a795  fmulp    st(1)                          
0x01a797  fld      dword ptr [ebp - 0x90]         
0x01a79d  fld      dword ptr [ebx - 0x15004]        ; .rodata 0x073d94 = -1
0x01a7a3  fmulp    st(1)                          
0x01a7a5  fsubp    st(1)                          
0x01a7a7  fstp     dword ptr [ebp - 0x3c]         
0x01a7aa  fld      dword ptr [ebp - 0x90]         
0x01a7b0  fld      dword ptr [ebx - 0x15000]        ; .rodata 0x073d98 = -0.2
0x01a7b6  fmulp    st(1)                          
0x01a7b8  fstp     dword ptr [ebp - 0x38]         
0x01a7bb  jmp      0x1a9c5                        
0x01a7c0  fld      dword ptr [ebp - 0x7c]         
0x01a7c3  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a7c9  fxch     st(1)                          
0x01a7cb  fucompp                                 
0x01a7cd  fnstsw   ax                             
0x01a7cf  sahf                                    
0x01a7d0  jne      0x1a7d9                        
0x01a7d2  jp       0x1a7d9                        
0x01a7d4  jmp      0x1a8b7                        
0x01a7d9  mov      eax, dword ptr [ebp + 0xc]     
0x01a7dc  mov      eax, dword ptr [eax + 8]       
0x01a7df  and      eax, 0x20                      
0x01a7e2  test     eax, eax                       
0x01a7e4  je       0x1a851                        
0x01a7e6  fld      dword ptr [ebp - 0x7c]         
0x01a7e9  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a7ef  fxch     st(1)                          
0x01a7f1  fucompp                                 
0x01a7f3  fnstsw   ax                             
0x01a7f5  sahf                                    
0x01a7f6  ja       0x1a7fa                        
0x01a7f8  jmp      0x1a827                        
0x01a7fa  fld      dword ptr [ebp - 0x90]         
0x01a800  fld      dword ptr [ebx - 0x14ffc]        ; .rodata 0x073d9c = 1.5
0x01a806  fmulp    st(1)                          
0x01a808  fstp     dword ptr [ebp - 0x90]         
0x01a80e  fld      dword ptr [ebp - 0xa0]         
0x01a814  fld      dword ptr [ebx - 0x14ffc]        ; .rodata 0x073d9c = 1.5
0x01a81a  fmulp    st(1)                          
0x01a81c  fstp     dword ptr [ebp - 0xa0]         
0x01a822  jmp      0x1a8b7                        
0x01a827  fld      dword ptr [ebp - 0x90]         
0x01a82d  fld      dword ptr [ebx - 0x14ff8]        ; .rodata 0x073da0 = 1.25
0x01a833  fmulp    st(1)                          
0x01a835  fstp     dword ptr [ebp - 0x90]         
0x01a83b  fld      dword ptr [ebp - 0xa0]         
0x01a841  fld      dword ptr [ebx - 0x14ff8]        ; .rodata 0x073da0 = 1.25
0x01a847  fmulp    st(1)                          
0x01a849  fstp     dword ptr [ebp - 0xa0]         
0x01a84f  jmp      0x1a8b7                        
0x01a851  fld      dword ptr [ebp - 0x7c]         
0x01a854  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a85a  fxch     st(1)                          
0x01a85c  fucompp                                 
0x01a85e  fnstsw   ax                             
0x01a860  sahf                                    
0x01a861  ja       0x1a865                        
0x01a863  jmp      0x1a88f                        
0x01a865  fld      dword ptr [ebp - 0x90]         
0x01a86b  fld      dword ptr [ebx - 0x15008]        ; .rodata 0x073d90 = 0.8
0x01a871  fmulp    st(1)                          
0x01a873  fstp     dword ptr [ebp - 0x90]         
0x01a879  fld      dword ptr [ebp - 0xa0]         
0x01a87f  fld      dword ptr [ebx - 0x15008]        ; .rodata 0x073d90 = 0.8
0x01a885  fmulp    st(1)                          
0x01a887  fstp     dword ptr [ebp - 0xa0]         
0x01a88d  jmp      0x1a8b7                        
0x01a88f  fld      dword ptr [ebp - 0x90]         
0x01a895  fld      dword ptr [ebx - 0x14ff8]        ; .rodata 0x073da0 = 1.25
0x01a89b  fmulp    st(1)                          
0x01a89d  fstp     dword ptr [ebp - 0x90]         
0x01a8a3  fld      dword ptr [ebp - 0xa0]         
0x01a8a9  fld      dword ptr [ebx - 0x14ff8]        ; .rodata 0x073da0 = 1.25
0x01a8af  fmulp    st(1)                          
0x01a8b1  fstp     dword ptr [ebp - 0xa0]         
0x01a8b7  fld      dword ptr [ebp - 0x7c]         
0x01a8ba  fmul     dword ptr [ebx - 0x15068]        ; .rodata 0x073d30 = 50
0x01a8c0  fld      dword ptr [ebx - 0x14ff4]        ; .rodata 0x073da4 = 0.075
0x01a8c6  fmulp    st(1)                          
0x01a8c8  fld      dword ptr [ebp - 0x60]         
0x01a8cb  faddp    st(1)                          
0x01a8cd  fstp     dword ptr [ebp - 0x60]         
0x01a8d0  fld      dword ptr [ebp - 0x98]         
0x01a8d6  fld      dword ptr [ebx - 0x1500c]        ; .rodata 0x073d8c = 0.2
0x01a8dc  fmulp    st(1)                          
0x01a8de  fstp     dword ptr [ebp - 0x58]         
0x01a8e1  fld      dword ptr [ebp - 0x94]         
0x01a8e7  fld      dword ptr [ebx - 0x14ff0]        ; .rodata 0x073da8 = 0.4
0x01a8ed  fmulp    st(1)                          
0x01a8ef  fstp     dword ptr [ebp - 0x54]         
0x01a8f2  fld      dword ptr [ebp - 0x90]         
0x01a8f8  fld      dword ptr [ebx - 0x15048]        ; .rodata 0x073d50 = 0.5
0x01a8fe  fmulp    st(1)                          
0x01a900  fstp     dword ptr [ebp - 0x50]         
0x01a903  mov      eax, dword ptr [ebp + 0xc]     
0x01a906  fld      dword ptr [eax + 0xe8]         
0x01a90c  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a912  fxch     st(1)                          
0x01a914  fucompp                                 
0x01a916  fnstsw   ax                             
0x01a918  sahf                                    
0x01a919  jne      0x1a939                        
0x01a91b  jp       0x1a939                        
0x01a91d  mov      eax, dword ptr [ebp + 0xc]     
0x01a920  fld      dword ptr [eax + 0xec]         
0x01a926  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01a92c  fxch     st(1)                          
0x01a92e  fucompp                                 
0x01a930  fnstsw   ax                             
0x01a932  sahf                                    
0x01a933  jne      0x1a939                        
0x01a935  jp       0x1a939                        
0x01a937  jmp      0x1a95f                        
0x01a939  mov      eax, dword ptr [ebp + 0xc]     
0x01a93c  mov      eax, dword ptr [eax + 0xec]    
0x01a942  mov      dword ptr [esp + 4], eax       
0x01a946  mov      eax, dword ptr [ebp + 0xc]     
0x01a949  mov      eax, dword ptr [eax + 0xe8]    
0x01a94f  mov      dword ptr [esp], eax           
0x01a952  call     0x14450                        
0x01a957  fld      dword ptr [ebp - 0x58]         
0x01a95a  faddp    st(1)                          
0x01a95c  fstp     dword ptr [ebp - 0x58]         
0x01a95f  fld      dword ptr [ebp - 0x98]         
0x01a965  fld      dword ptr [ebx - 0x15014]        ; .rodata 0x073d84 = 0.3
0x01a96b  fmulp    st(1)                          
0x01a96d  fstp     dword ptr [ebp - 0x4c]         
0x01a970  fld      dword ptr [ebp - 0x94]         
0x01a976  fld      dword ptr [ebx - 0x14ff0]        ; .rodata 0x073da8 = 0.4
0x01a97c  fmulp    st(1)                          
0x01a97e  fstp     dword ptr [ebp - 0x48]         
0x01a981  fld      dword ptr [ebp - 0x90]         
0x01a987  fld      dword ptr [ebx - 0x15048]        ; .rodata 0x073d50 = 0.5
0x01a98d  fmulp    st(1)                          
0x01a98f  fstp     dword ptr [ebp - 0x44]         
0x01a992  fld      dword ptr [ebp - 0x98]         
0x01a998  fld      dword ptr [ebx - 0x15048]        ; .rodata 0x073d50 = 0.5
0x01a99e  fmulp    st(1)                          
0x01a9a0  fstp     dword ptr [ebp - 0x40]         
0x01a9a3  fld      dword ptr [ebp - 0x94]         
0x01a9a9  fld      dword ptr [ebx - 0x1500c]        ; .rodata 0x073d8c = 0.2
0x01a9af  fmulp    st(1)                          
0x01a9b1  fstp     dword ptr [ebp - 0x3c]         
0x01a9b4  fld      dword ptr [ebp - 0x90]         
0x01a9ba  fld      dword ptr [ebx - 0x14fec]        ; .rodata 0x073dac = -0.6
0x01a9c0  fmulp    st(1)                          
0x01a9c2  fstp     dword ptr [ebp - 0x38]         
0x01a9c5  fld      dword ptr [ebp - 0xa8]         
0x01a9cb  fld      dword ptr [ebx - 0x15014]        ; .rodata 0x073d84 = 0.3
0x01a9d1  fmulp    st(1)                          
0x01a9d3  fstp     dword ptr [ebp - 0x34]         
0x01a9d6  fld      dword ptr [ebp - 0xa4]         
0x01a9dc  fld      dword ptr [ebx - 0x15014]        ; .rodata 0x073d84 = 0.3
0x01a9e2  fmulp    st(1)                          
0x01a9e4  fstp     dword ptr [ebp - 0x30]         
0x01a9e7  mov      dword ptr [ebp - 0x2c], 0      
0x01a9ee  fld      dword ptr [ebp - 0xa8]         
0x01a9f4  fld      dword ptr [ebx - 0x14fe8]        ; .rodata 0x073db0 = 0.7
0x01a9fa  fmulp    st(1)                          
0x01a9fc  fstp     dword ptr [ebp - 0x28]         
0x01a9ff  fld      dword ptr [ebp - 0xa4]         
0x01aa05  fld      dword ptr [ebx - 0x14fe8]        ; .rodata 0x073db0 = 0.7
0x01aa0b  fmulp    st(1)                          
0x01aa0d  fstp     dword ptr [ebp - 0x24]         
0x01aa10  fld      dword ptr [ebp - 0xa0]         
0x01aa16  fld      dword ptr [ebx - 0x14fe4]        ; .rodata 0x073db4 = -0.3
0x01aa1c  fmulp    st(1)                          
0x01aa1e  fstp     dword ptr [ebp - 0x20]         
0x01aa21  mov      eax, dword ptr [ebx - 0x1504c]   ; .rodata 0x073d4c = 0
0x01aa27  mov      dword ptr [ebp - 0x14], eax    
0x01aa2a  mov      dword ptr [ebp - 0x18], eax    
0x01aa2d  mov      dword ptr [ebp - 0x1c], eax    
0x01aa30  mov      eax, dword ptr [ebp + 0xc]     
0x01aa33  fld      dword ptr [eax + 0xec]         
0x01aa39  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01aa3f  fxch     st(1)                          
0x01aa41  fucompp                                 
0x01aa43  fnstsw   ax                             
0x01aa45  sahf                                    
0x01aa46  jne      0x1aa66                        
0x01aa48  jp       0x1aa66                        
0x01aa4a  mov      eax, dword ptr [ebp + 0xc]     
0x01aa4d  fld      dword ptr [eax + 0xe8]         
0x01aa53  fld      dword ptr [ebx - 0x1504c]        ; .rodata 0x073d4c = 0
0x01aa59  fxch     st(1)                          
0x01aa5b  fucompp                                 
0x01aa5d  fnstsw   ax                             
0x01aa5f  sahf                                    
0x01aa60  jne      0x1aa66                        
0x01aa62  jp       0x1aa66                        
0x01aa64  jmp      0x1aa87                        
0x01aa66  mov      eax, dword ptr [ebp + 0xc]     
0x01aa69  mov      eax, dword ptr [eax + 0xe8]    
0x01aa6f  mov      dword ptr [esp + 4], eax       
0x01aa73  mov      eax, dword ptr [ebp + 0xc]     
0x01aa76  mov      eax, dword ptr [eax + 0xec]    
0x01aa7c  mov      dword ptr [esp], eax           
0x01aa7f  call     0x14450                        
0x01aa84  fstp     dword ptr [ebp - 0x1c]         
0x01aa87  mov      dword ptr [ebp - 0xb8], 0      
0x01aa91  cmp      dword ptr [ebp - 0xb8], 5      
0x01aa98  jle      0x1aa9f                        
0x01aa9a  jmp      0x1ab42                        
0x01aa9f  mov      esi, dword ptr [ebp + 0x18]    
0x01aaa2  mov      edx, dword ptr [ebp - 0xb8]    
0x01aaa8  mov      eax, edx                       
0x01aaaa  add      eax, eax                       
0x01aaac  add      eax, edx                       
0x01aaae  lea      ecx, [eax*4]                   
0x01aab5  mov      edx, dword ptr [ebp - 0xb8]    
0x01aabb  mov      eax, edx                       
0x01aabd  add      eax, eax                       
0x01aabf  add      eax, edx                       
0x01aac1  shl      eax, 2                         
0x01aac4  lea      edx, [ebp - 8]                 
0x01aac7  add      eax, edx                       
0x01aac9  sub      eax, 0x50                      
0x01aacc  mov      eax, dword ptr [eax]           
0x01aace  mov      dword ptr [esi + ecx], eax     
0x01aad1  mov      ecx, dword ptr [ebp + 0x18]    
0x01aad4  mov      edx, dword ptr [ebp - 0xb8]    
0x01aada  mov      eax, edx                       
0x01aadc  add      eax, eax                       
0x01aade  add      eax, edx                       
0x01aae0  shl      eax, 2                         
0x01aae3  add      eax, ecx                       
0x01aae5  lea      ecx, [eax + 4]                 
0x01aae8  mov      edx, dword ptr [ebp - 0xb8]    
0x01aaee  mov      eax, edx                       
0x01aaf0  add      eax, eax                       
0x01aaf2  add      eax, edx                       
0x01aaf4  shl      eax, 2                         
0x01aaf7  lea      edx, [ebp - 8]                 
0x01aafa  add      eax, edx                       
0x01aafc  sub      eax, 0x4c                      
0x01aaff  mov      eax, dword ptr [eax]           
0x01ab01  mov      dword ptr [ecx], eax           
0x01ab03  mov      ecx, dword ptr [ebp + 0x18]    
0x01ab06  mov      edx, dword ptr [ebp - 0xb8]    
0x01ab0c  mov      eax, edx                       
0x01ab0e  add      eax, eax                       
0x01ab10  add      eax, edx                       
0x01ab12  shl      eax, 2                         
0x01ab15  add      eax, ecx                       
0x01ab17  lea      ecx, [eax + 8]                 
0x01ab1a  mov      edx, dword ptr [ebp - 0xb8]    
0x01ab20  mov      eax, edx                       
0x01ab22  add      eax, eax                       
0x01ab24  add      eax, edx                       
0x01ab26  shl      eax, 2                         
0x01ab29  lea      edx, [ebp - 8]                 
0x01ab2c  add      eax, edx                       
0x01ab2e  sub      eax, 0x48                      
0x01ab31  mov      eax, dword ptr [eax]           
0x01ab33  mov      dword ptr [ecx], eax           
0x01ab35  lea      eax, [ebp - 0xb8]              
0x01ab3b  inc      dword ptr [eax]                
0x01ab3d  jmp      0x1aa91                        
0x01ab42  mov      edx, dword ptr [ebp + 0x18]    
0x01ab45  mov      eax, dword ptr [ebp - 0x68]    
0x01ab48  mov      dword ptr [edx + 0x48], eax    
0x01ab4b  mov      edx, dword ptr [ebp + 0x18]    
0x01ab4e  mov      eax, dword ptr [ebp - 0x64]    
0x01ab51  mov      dword ptr [edx + 0x4c], eax    
0x01ab54  mov      edx, dword ptr [ebp + 0x18]    
0x01ab57  mov      eax, dword ptr [ebp - 0x60]    
0x01ab5a  mov      dword ptr [edx + 0x50], eax    
0x01ab5d  mov      edx, dword ptr [ebp + 0x18]    
0x01ab60  mov      eax, dword ptr [ebp - 0x78]    
0x01ab63  mov      dword ptr [edx + 0x54], eax    
0x01ab66  mov      edx, dword ptr [ebp + 0x18]    
0x01ab69  mov      eax, dword ptr [ebp - 0x74]    
0x01ab6c  mov      dword ptr [edx + 0x58], eax    
0x01ab6f  mov      edx, dword ptr [ebp + 0x18]    
0x01ab72  mov      eax, dword ptr [ebp - 0x70]    
0x01ab75  mov      dword ptr [edx + 0x5c], eax    
0x01ab78  add      esp, 0xe0                      
0x01ab7e  pop      ebx                            
0x01ab7f  pop      esi                            
0x01ab80  pop      ebp                            
0x01ab81  ret                                     
0x01ab82  push     ebp                            
0x01ab83  mov      ebp, esp                       
0x01ab85  push     esi                            
0x01ab86  sub      esp, 0xc                       
0x01ab89  mov      dword ptr [ebp - 0xc], 0       
0x01ab90  cmp      dword ptr [ebp - 0xc], 2       
0x01ab94  jle      0x1ab9b                        
0x01ab96  jmp      0x1ac5c                        
0x01ab9b  mov      eax, dword ptr [ebp - 0xc]     
0x01ab9e  lea      esi, [eax*4]                   
0x01aba5  mov      ecx, dword ptr [ebp + 8]       
0x01aba8  mov      eax, dword ptr [ebp - 0xc]     
0x01abab  lea      edx, [eax*4]                   
0x01abb2  mov      eax, dword ptr [ebp + 0x10]    
0x01abb5  fld      dword ptr [ecx + esi]          
0x01abb8  fsub     dword ptr [eax + edx]          
0x01abbb  fstp     dword ptr [ebp - 8]            
0x01abbe  fld      dword ptr [ebp - 8]            
0x01abc1  fld      dword ptr [ebp + 0xc]          
0x01abc4  fxch     st(1)                          
0x01abc6  fucompp                                 
0x01abc8  fnstsw   ax                             
0x01abca  sahf                                    
0x01abcb  ja       0x1abcf                        
0x01abcd  jmp      0x1abf4                        
0x01abcf  mov      eax, dword ptr [ebp - 0xc]     
0x01abd2  lea      ecx, [eax*4]                   
0x01abd9  mov      esi, dword ptr [ebp + 0x10]    
0x01abdc  mov      eax, dword ptr [ebp - 0xc]     
0x01abdf  lea      edx, [eax*4]                   
0x01abe6  mov      eax, dword ptr [ebp + 0x10]    
0x01abe9  fld      dword ptr [eax + edx]          
0x01abec  fadd     dword ptr [ebp + 0xc]          
0x01abef  fstp     dword ptr [esi + ecx]          
0x01abf2  jmp      0x1ac52                        
0x01abf4  mov      eax, dword ptr [ebp + 0xc]     
0x01abf7  mov      dword ptr [ebp - 0x10], eax    
0x01abfa  fld      dword ptr [ebp - 0x10]         
0x01abfd  fchs                                    
0x01abff  fld      dword ptr [ebp - 8]            
0x01ac02  fxch     st(1)                          
0x01ac04  fucompp                                 
0x01ac06  fnstsw   ax                             
0x01ac08  sahf                                    
0x01ac09  ja       0x1ac0d                        
0x01ac0b  jmp      0x1ac32                        
0x01ac0d  mov      eax, dword ptr [ebp - 0xc]     
0x01ac10  lea      ecx, [eax*4]                   
0x01ac17  mov      esi, dword ptr [ebp + 0x10]    
0x01ac1a  mov      eax, dword ptr [ebp - 0xc]     
0x01ac1d  lea      edx, [eax*4]                   
0x01ac24  mov      eax, dword ptr [ebp + 0x10]    
0x01ac27  fld      dword ptr [eax + edx]          
0x01ac2a  fsub     dword ptr [ebp + 0xc]          
0x01ac2d  fstp     dword ptr [esi + ecx]          
0x01ac30  jmp      0x1ac52                        
0x01ac32  mov      eax, dword ptr [ebp - 0xc]     
0x01ac35  lea      esi, [eax*4]                   
0x01ac3c  mov      ecx, dword ptr [ebp + 0x10]    
0x01ac3f  mov      eax, dword ptr [ebp - 0xc]     
0x01ac42  lea      edx, [eax*4]                   
0x01ac49  mov      eax, dword ptr [ebp + 8]       
0x01ac4c  mov      eax, dword ptr [eax + edx]     
0x01ac4f  mov      dword ptr [ecx + esi], eax     
0x01ac52  lea      eax, [ebp - 0xc]               
0x01ac55  inc      dword ptr [eax]                
0x01ac57  jmp      0x1ab90                        
0x01ac5c  add      esp, 0xc                       
0x01ac5f  pop      esi                            
0x01ac60  pop      ebp                            
0x01ac61  ret                                     
0x01ac62  push     ebp                            
0x01ac63  mov      ebp, esp                       
0x01ac65  push     ebx                            
0x01ac66  sub      esp, 0x34                      
0x01ac69  call     0x1b47c                        
0x01ac6e  add      ebx, 0x6e12a                   
0x01ac74  mov      eax, dword ptr [ebp + 8]       
0x01ac77  mov      edx, dword ptr [ebp + 0x10]    
0x01ac7a  fld      dword ptr [eax]                
0x01ac7c  fsub     dword ptr [edx]                
0x01ac7e  fstp     dword ptr [ebp - 0x18]         
0x01ac81  mov      eax, dword ptr [ebp + 8]       
0x01ac84  add      eax, 4                         
0x01ac87  mov      edx, dword ptr [ebp + 0x10]    
0x01ac8a  add      edx, 4                         
0x01ac8d  fld      dword ptr [eax]                
0x01ac8f  fsub     dword ptr [edx]                
0x01ac91  fstp     dword ptr [ebp - 0x14]         
0x01ac94  mov      eax, dword ptr [ebp + 8]       
0x01ac97  add      eax, 8                         
0x01ac9a  mov      edx, dword ptr [ebp + 0x10]    
0x01ac9d  add      edx, 8                         
0x01aca0  fld      dword ptr [eax]                
0x01aca2  fsub     dword ptr [edx]                
0x01aca4  fstp     dword ptr [ebp - 0x10]         
0x01aca7  fld      dword ptr [ebp - 0x18]         
0x01acaa  fmul     dword ptr [ebp - 0x18]         
0x01acad  fld      dword ptr [ebp - 0x14]         
0x01acb0  fmul     dword ptr [ebp - 0x14]         
0x01acb3  faddp    st(1)                          
0x01acb5  fld      dword ptr [ebp - 0x10]         
0x01acb8  fmul     dword ptr [ebp - 0x10]         
0x01acbb  faddp    st(1)                          
0x01acbd  fstp     dword ptr [ebp - 0x1c]         
0x01acc0  fld      dword ptr [ebp - 0x1c]         
0x01acc3  fld      dword ptr [ebx - 0x14fe0]        ; .rodata 0x073db8 = 0
0x01acc9  fxch     st(1)                          
0x01accb  fucompp                                 
0x01accd  fnstsw   ax                             
0x01accf  sahf                                    
0x01acd0  jp       0x1acd8                        
0x01acd2  je       0x1ad6b                        
0x01acd8  mov      eax, dword ptr [ebp - 0x1c]    
0x01acdb  mov      dword ptr [esp], eax           
0x01acde  call     0x151c0                        
0x01ace3  fld      dword ptr [ebp + 0xc]          
0x01ace6  fmulp    st(1)                          
0x01ace8  fstp     dword ptr [ebp - 0x1c]         
0x01aceb  fld      dword ptr [ebp - 0x1c]         
0x01acee  fld      dword ptr [ebx - 0x14fdc]        ; .rodata 0x073dbc = 1
0x01acf4  fucompp                                 
0x01acf6  fnstsw   ax                             
0x01acf8  sahf                                    
0x01acf9  ja       0x1acfd                        
0x01acfb  jmp      0x1ad41                        
0x01acfd  mov      edx, dword ptr [ebp + 0x10]    
0x01ad00  mov      eax, dword ptr [ebp + 0x10]    
0x01ad03  fld      dword ptr [ebp - 0x18]         
0x01ad06  fmul     dword ptr [ebp - 0x1c]         
0x01ad09  fld      dword ptr [eax]                
0x01ad0b  faddp    st(1)                          
0x01ad0d  fstp     dword ptr [edx]                
0x01ad0f  mov      edx, dword ptr [ebp + 0x10]    
0x01ad12  add      edx, 4                         
0x01ad15  mov      eax, dword ptr [ebp + 0x10]    
0x01ad18  add      eax, 4                         
0x01ad1b  fld      dword ptr [ebp - 0x14]         
0x01ad1e  fmul     dword ptr [ebp - 0x1c]         
0x01ad21  fld      dword ptr [eax]                
0x01ad23  faddp    st(1)                          
0x01ad25  fstp     dword ptr [edx]                
0x01ad27  mov      edx, dword ptr [ebp + 0x10]    
0x01ad2a  add      edx, 8                         
0x01ad2d  mov      eax, dword ptr [ebp + 0x10]    
0x01ad30  add      eax, 8                         
0x01ad33  fld      dword ptr [ebp - 0x10]         
0x01ad36  fmul     dword ptr [ebp - 0x1c]         
0x01ad39  fld      dword ptr [eax]                
0x01ad3b  faddp    st(1)                          
0x01ad3d  fstp     dword ptr [edx]                
0x01ad3f  jmp      0x1ad6b                        
0x01ad41  mov      edx, dword ptr [ebp + 0x10]    
0x01ad44  mov      eax, dword ptr [ebp + 8]       
0x01ad47  mov      eax, dword ptr [eax]           
0x01ad49  mov      dword ptr [edx], eax           
0x01ad4b  mov      edx, dword ptr [ebp + 0x10]    
0x01ad4e  add      edx, 4                         
0x01ad51  mov      eax, dword ptr [ebp + 8]       
0x01ad54  add      eax, 4                         
0x01ad57  mov      eax, dword ptr [eax]           
0x01ad59  mov      dword ptr [edx], eax           
0x01ad5b  mov      edx, dword ptr [ebp + 0x10]    
0x01ad5e  add      edx, 8                         
0x01ad61  mov      eax, dword ptr [ebp + 8]       
0x01ad64  add      eax, 8                         
0x01ad67  mov      eax, dword ptr [eax]           
0x01ad69  mov      dword ptr [edx], eax           
0x01ad6b  add      esp, 0x34                      
0x01ad6e  pop      ebx                            
0x01ad6f  pop      ebp                            
0x01ad70  ret                                     