# Xiao-esp32s3-lora-repeater
Xiao esp32s3 with dual SX1262 radio SPI crossband repeater.
<img width="4096" height="3265" alt="PXL_20260507_021829300~2" src="https://github.com/user-attachments/assets/b9e68624-3cb4-46a3-9c2f-4927e6a8fdf2" />
###### *touched by claude but not by epstein*
# Instructions 
1)  Adjust radio1 and radio2 parameters per protocol in [platformio.ini](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/2bfe2c5d7d75cf3592776caf77d9234c22970f26/platformio.ini#L19-L44) for repeater. Protocol filtered by ``` -DLORA_RADIO1_SYNC_WORD=0x2B```
2)  Flash
3)  Upload
4)  Refer to serial debug to monitor packets  
