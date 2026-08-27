# SPDX-FileCopyrightText: 2020 IAR Systems AB
# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
all:
	../../cttd.exe -conv -objd test-OBJD.txt -pcinfo ./output/test-PCINFO.txt
	../../cttd.exe -conv -pcinfo ./output/test-PCINFO.txt -pconly test-PCONLY.txt -pcseq ./output/test-PCSEQ.txt
	echo No Branch History Message - only Direct Branch Message
	../../cttd.exe -enco ./output/test-PCSEQ.txt -nex ./output/test-NEX.bin -nobhm
	../../cttd.exe -dump ./output/test-NEX.bin ./output/test-DUMP.txt
	../../cttd.exe -deco ./output/test-NEX.bin -pcinfo ./output/test-PCINFO.txt -pcout ./output/test-PCOUT.txt
	../../cttd.exe -diff -pconly ./test-PCONLY.txt -pcout ./output/test-PCOUT.txt	
	echo  No Call-stack and no Repeat Message ...
	../../cttd.exe -enco ./output/test-PCSEQ.txt -nex ./output/test-NEX.bin -cs 0 -rpt 0
	../../cttd.exe -dump ./output/test-NEX.bin ./output/test-DUMP.txt
	../../cttd.exe -deco ./output/test-NEX.bin -pcinfo ./output/test-PCINFO.txt -pcout ./output/test-PCOUT.txt
	../../cttd.exe -diff -pconly ./test-PCONLY.txt -pcout ./output/test-PCOUT.txt	
	echo  No Call-stack and with Repeat Message ...
	../../cttd.exe -enco ./output/test-PCSEQ.txt -nex ./output/test-NEX.bin  -cs 0 -rpt 2
	../../cttd.exe -dump ./output/test-NEX.bin ./output/test-DUMP.txt
	../../cttd.exe -deco ./output/test-NEX.bin -pcinfo ./output/test-PCINFO.txt -pcout ./output/test-PCOUT.txt
	../../cttd.exe -diff -pconly ./test-PCONLY.txt -pcout ./output/test-PCOUT.txt	
	echo  With callstack and no repeat
	../../cttd.exe -enco ./output/test-PCSEQ.txt -nex ./output/test-NEX.bin -cs 8 -rpt 0
	../../cttd.exe -dump ./output/test-NEX.bin ./output/test-DUMP.txt
	../../cttd.exe -deco ./output/test-NEX.bin -pcinfo ./output/test-PCINFO.txt -pcout ./output/test-PCOUT.txt
	../../cttd.exe -diff -pconly ./test-PCONLY.txt -pcout ./output/test-PCOUT.txt	
	echo  With callstack and repeat -best possible compression
	../../cttd.exe -enco ./output/test-PCSEQ.txt -nex ./output/test-NEX.bin -cs 8 -rpt 2
	../../cttd.exe -dump ./output/test-NEX.bin ./output/test-DUMP.txt
	../../cttd.exe -deco ./output/test-NEX.bin -pcinfo ./output/test-PCINFO.txt -pcout ./output/test-PCOUT.txt
	../../cttd.exe -diff -pconly ./test-PCONLY.txt -pcout ./output/test-PCOUT.txt	

ELF:
	riscv64-unknown-elf-gcc -c -g -fno-builtin -nostdlib -fsigned-char -g -ffunction-sections -fdata-sections -march=rv32imac -mabi=ilp32 -mcmodel=medlow test.c
	riscv64-unknown-elf-gcc -g -march=rv32imac -mabi=ilp32 -mcmodel=medlow -fno-builtin -nostdlib ./entry.S ./crt0.S test.o -Wl,--gc-sections -Wl,-Map,test.map -o test.elf -T./rom-20010000_ram-80000000.lds -Wl,--start-group -lc -lgcc -lm -Wl,--end-group
	
OBJD:
	riscv64-unknown-elf-objdump -d test.elf >test-OBJD.txt 
