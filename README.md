# CXLSHMSWcoherence

## Host Machine
- CXL memory를 devdax mode로 켜는 방법
    - `sudo vim /etc/default/grub` 으로 들어가서
    - `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash memmap=8G!8G modprobe.blacklist=kmem" # to change CXL as devdax mode` 추가
    - `sudo update-grub` 하고 reboot
    - devdax → system RAM 은 위에거 주석처리하면 됨.
 
- 확인하는법
  - `lsmod | grep kmem`: kmem module 실행 되었는지
  - `sudo grep '5080000000-687fffffff' /proc/iomem`: soft reserved 로만 잡히고, system RAM으로 안잡히는지
  - `daxctl list`: dax로 잘 잡혀있는지
  - `sudo daxctl enable-device dax1.0`: devdax device enabling

- devdax mode의 CXL smoke test
  - 경로: `/home/comsys/sungsu/CXL_shared_mem/dax_setting`
  - 동작 확인
      
      ```bash- devdax mode의 CXL smoke test
  - 경로: `/home/comsys/sungsu/CXL_shared_mem/dax_setting`
  - 동작 확인
      
      ```bash
      comsys@clou- devdax mode의 CXL smoke test
  - 동작 확인
      
      ```bash
      comsys@cloudcxl2:~/sungsu/CXL_shared_mem/dax_setting$ gcc -o dax_test dax_test.c
      comsys@cloudcxl2:~/sungsu/CXL_shared_mem/dax_setting$ sudo ./dax_test
      [INFO] Opening /dev/dax1.0...
      [OK] Opened /dev/dax1.0
      [INFO] mmap() 2097152 bytes...
      [OK] mmap succeeded at address 0x7fc02f800000
      [INFO] Writing message to DAX memory...
      [OK] Message written: "Hello from DAX mmap with flush!
      "
      [INFO] Flushing changes with clflush + sfence...
      [OK] Flush successful
      [INFO] Cleaning up...
      [DONE] All operations completed successfully.
      ```dcxl2:~/sungsu/CXL_shared_mem/dax_setting$ gcc -o dax_test dax_test.c
      comsys@cloudcxl2:~/sungsu/CXL_shared_mem/dax_setting$ sudo ./dax_test
      [INFO] Opening /dev/dax1.0...
      [OK] Opened /dev/dax1.0
      [INFO] mmap() 2097152 bytes...
      [OK] mmap succeeded at address 0x7fc02f800000
      [INFO] Writing message to DAX memory...
      [OK] Message written: "Hello from DAX mmap with flush!
      "
      [INFO] Flushing changes with clflush + sfence...
      [OK] Flush successful
      [INFO] Cleaning up...
      [DONE] All operations completed successfully.
      ```
        [INFO] Cleaning up...
        [DONE] All operations completed successfully.
        ```
