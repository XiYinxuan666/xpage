#include  <Uefi.h>
#include  <Library/UefiLib.h>
#include  <Library/UefiBootServicesTableLib.h>
#include  <Library/PrintLib.h>
#include  <Library/MemoryAllocationLib.h>
#include  <Library/BaseMemoryLib.h>
#include  <Protocol/LoadedImage.h>
#include  <Protocol/SimpleFileSystem.h>
#include  <Protocol/DiskIo2.h>
#include  <Protocol/BlockIo.h>
#include  <Protocol/GraphicsOutput.h>
#include  <Guid/FileInfo.h>
#include  "frame_buffer_config.hpp"
#include  "elf.hpp"

//MemoryMap结构体定义
struct MemoryMap {
  UINTN buffer_size;
  VOID* buffer;
  UINTN map_size;
  UINTN map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
};

//获取内存映射函数
EFI_STATUS GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }

  map->map_size = map->buffer_size;
  return gBS->GetMemoryMap(
      &map->map_size,
      (EFI_MEMORY_DESCRIPTOR*)map->buffer,
      &map->map_key,
      &map->descriptor_size,
      &map->descriptor_version);
}

//获取内存类型对应的字符串函数
const CHAR16* GetMemoryTypeUnicode(EFI_MEMORY_TYPE type) {
  switch (type) {
    case EfiReservedMemoryType: return L"EfiReservedMemoryType";
    case EfiLoaderCode: return L"EfiLoaderCode";
    case EfiLoaderData: return L"EfiLoaderData";
    case EfiBootServicesCode: return L"EfiBootServicesCode";
    case EfiBootServicesData: return L"EfiBootServicesData";
    case EfiRuntimeServicesCode: return L"EfiRuntimeServicesCode";
    case EfiRuntimeServicesData: return L"EfiRuntimeServicesData";
    case EfiConventionalMemory: return L"EfiConventionalMemory";
    case EfiUnusableMemory: return L"EfiUnusableMemory";
    case EfiACPIReclaimMemory: return L"EfiACPIReclaimMemory";
    case EfiACPIMemoryNVS: return L"EfiACPIMemoryNVS";
    case EfiMemoryMappedIO: return L"EfiMemoryMappedIO";
    case EfiMemoryMappedIOPortSpace: return L"EfiMemoryMappedIOPortSpace";
    case EfiPalCode: return L"EfiPalCode";
    case EfiPersistentMemory: return L"EfiPersistentMemory";
    case EfiMaxMemoryType: return L"EfiMaxMemoryType";
    default: return L"InvalidMemoryType";
  }
}

//保存内存映射到文件函数
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  CHAR8 buf[256];
  UINTN len;

  CHAR8* header =
    "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  file->Write(file, &len, header);

  Print(L"map->buffer = %08lx, map->map_size = %08lx\n",
      map->buffer, map->map_size);

  EFI_PHYSICAL_ADDRESS iter;
  int i;
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
    len = AsciiSPrint(
        buf, sizeof(buf),
        "%u, %x, %-ls, %08lx, %lx, %lx\n",
        i, desc->Type, GetMemoryTypeUnicode(desc->Type),
        desc->PhysicalStart, desc->NumberOfPages,
        desc->Attribute & 0xffffflu);
    file->Write(file, &len, buf);
  }

  return EFI_SUCCESS;
}

//打开根目录函数
EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;

  gBS->OpenProtocol(
      image_handle,
      &gEfiLoadedImageProtocolGuid,
      (VOID**)&loaded_image,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

  gBS->OpenProtocol(
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&fs,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

  fs->OpenVolume(fs, root);

  return EFI_SUCCESS;
}

// 打开 GOP 图形输出协议
EFI_STATUS OpenGOP(EFI_HANDLE image_handle,
                   EFI_GRAPHICS_OUTPUT_PROTOCOL** gop) {
  UINTN num_gop_handles = 0;
  EFI_HANDLE* gop_handles = NULL;
  
  gBS->LocateHandleBuffer(
      ByProtocol,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      &num_gop_handles,
      &gop_handles);

  if (num_gop_handles == 0) {
    return EFI_UNSUPPORTED;
  }

  gBS->OpenProtocol(
      gop_handles[0],
      &gEfiGraphicsOutputProtocolGuid,
      (VOID**)gop,
      image_handle,
      NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

  gBS->FreePool(gop_handles);

  return EFI_SUCCESS;
}

// ---------------------------------------------------------
// 双缓冲支持
// ---------------------------------------------------------
EFI_GRAPHICS_OUTPUT_BLT_PIXEL* gBackBuffer = NULL;
UINT32 gBackBufferWidth = 0;
UINT32 gBackBufferHeight = 0;
BOOLEAN gUseBackBuffer = FALSE;

void InitBackBuffer(UINT32 width, UINT32 height) {
  if (gBackBuffer) {
    gBS->FreePool(gBackBuffer);
  }
  gBackBufferWidth = width;
  gBackBufferHeight = height;
  gBS->AllocatePool(EfiLoaderData, width * height * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL), (VOID**)&gBackBuffer);
}

void SyncBackBuffer(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop) {
  if (gBackBuffer) {
    gop->Blt(gop, gBackBuffer, EfiBltBufferToVideo, 0, 0, 0, 0, gBackBufferWidth, gBackBufferHeight, 0);
  }
}

// 绘制矩形函数 (支持双缓冲和硬件加速)
void DrawRect(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
              UINT32 x, UINT32 y, UINT32 w, UINT32 h,
              EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
  
  // 如果启用了双缓冲且缓冲区存在
  if (gUseBackBuffer && gBackBuffer) {
     if (x >= gBackBufferWidth || y >= gBackBufferHeight) return;
     // 裁剪边界
     UINT32 draw_w = (x + w > gBackBufferWidth) ? (gBackBufferWidth - x) : w;
     UINT32 draw_h = (y + h > gBackBufferHeight) ? (gBackBufferHeight - y) : h;
     
     for (UINT32 j = 0; j < draw_h; j++) {
       for (UINT32 i = 0; i < draw_w; i++) {
         gBackBuffer[(y + j) * gBackBufferWidth + (x + i)] = color;
       }
     }
     return;
  }

  // 直接绘制到屏幕：使用 Blt VideoFill (硬件加速)
  gop->Blt(gop, &color, EfiBltVideoFill, 0, 0, x, y, w, h, 0);
}

// ---------------------------------------------------------
// 数学与动画辅助函数
// ---------------------------------------------------------
#define PI 3.14159265359

double Sin(double x) {
  while (x > PI) x -= 2 * PI;
  while (x < -PI) x += 2 * PI;
  
  double res = 0;
  double term = x;
  double x2 = x * x;
  
  res += term;
  term *= -x2 / (2 * 3);
  res += term;
  term *= -x2 / (4 * 5);
  res += term;
  term *= -x2 / (6 * 7);
  res += term;
  
  return res;
}

double Cos(double x) {
  return Sin(x + PI / 2);
}

void DrawCircleFilled(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                      int cx, int cy, int r,
                      EFI_GRAPHICS_OUTPUT_BLT_PIXEL color) {
  for (int y = -r; y <= r; y++) {
    int start_x = 0;
    int width = 0;
    
    for (int x = -r; x <= r; x++) {
      if (x * x + y * y <= r * r) {
        if (width == 0) start_x = x;
        width++;
      } else if (width > 0) {
        break; 
      }
    }
    
    if (width > 0) {
      DrawRect(gop, cx + start_x, cy + y, width, 1, color);
    }
  }
}

UINT16 font_data[][16] = {
  // Y (0)
  {0x0000, 0x0000, 0xC300, 0x6600, 0x3C00, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x0000, 0x0000, 0x0000},
  // i (1)
  {0x0000, 0x1800, 0x1800, 0x0000, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x0000, 0x0000, 0x0000},
  // n (2)
  {0x0000, 0x0000, 0x0000, 0x0000, 0xDC00, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x0000, 0x0000, 0x0000},
  // x (3)
  {0x0000, 0x0000, 0x0000, 0x0000, 0xC300, 0x6600, 0x3C00, 0x1800, 0x3C00, 0x6600, 0xC300, 0xC300, 0xC300, 0x0000, 0x0000, 0x0000},
  // u (4)
  {0x0000, 0x0000, 0x0000, 0x0000, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x3E00, 0x0000, 0x0000, 0x0000},
  // a (5)
  {0x0000, 0x0000, 0x0000, 0x0000, 0x3C00, 0x0600, 0x3E00, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x3E00, 0x0000, 0x0000, 0x0000},
  // S (6)
  {0x0000, 0x0000, 0x3C00, 0x6600, 0x6000, 0x3000, 0x1800, 0x0C00, 0x0600, 0x0600, 0x6600, 0x3C00, 0x0000, 0x0000, 0x0000, 0x0000},
  // t (7)
  {0x0000, 0x0000, 0x1800, 0x1800, 0x7E00, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x1800, 0x0E00, 0x0000, 0x0000, 0x0000},
  // d (8)
  {0x0000, 0x0000, 0x0600, 0x0600, 0x0600, 0x3E00, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x3E00, 0x0000, 0x0000, 0x0000},
  // o (9)
  {0x0000, 0x0000, 0x0000, 0x0000, 0x3C00, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x6600, 0x3C00, 0x0000, 0x0000, 0x0000},
  // space (10)
  {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}
};

int title_indices[] = {0, 1, 2, 3, 4, 5, 2, 10, 6, 7, 4, 8, 1, 9};
int title_len = 14;

void DrawTitle(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, int cx, int cy) {
  int pixel_scale = 6;
  int char_width = 8;
  int char_height = 16;
  int total_width = title_len * char_width * pixel_scale;
  int start_x = cx - total_width / 2;
  int current_x = start_x;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL white = {255, 255, 255, 0};
  
  for (int k = 0; k < title_len; k++) {
    int idx = title_indices[k];
    for (int row = 0; row < char_height; row++) {
      UINT16 row_data = font_data[idx][row];
      for (int col = 0; col < char_width; col++) {
        if ((row_data >> (15 - col)) & 0x01) {
          DrawRect(gop, current_x + col * pixel_scale, cy + row * pixel_scale, pixel_scale, pixel_scale, white);
        }
      }
    }
    current_x += char_width * pixel_scale;
  }
}

void DrawDesktop(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop) {
  UINT32 width = gop->Mode->Info->HorizontalResolution;
  UINT32 height = gop->Mode->Info->VerticalResolution;

  InitBackBuffer(width, height);
  gUseBackBuffer = TRUE;

  EFI_GRAPHICS_OUTPUT_BLT_PIXEL bg_color = {60, 20, 220, 0};
  DrawRect(gop, 0, 0, width, height, bg_color);
  DrawTitle(gop, width / 2, height / 2 - 50);
  SyncBackBuffer(gop);

  EFI_GRAPHICS_OUTPUT_BLT_PIXEL white = {255, 255, 255, 0};
  int center_x = width / 2;
  int center_y = height / 2 + 100;
  int radius = 30;
  int num_dots = 8;
  double angle_step = 2 * PI / num_dots;
  
  for (int frame = 0; frame < 60; frame++) {
    DrawRect(gop, center_x - radius - 10, center_y - radius - 10, (radius + 10) * 2, (radius + 10) * 2, bg_color);
    for (int i = 0; i < num_dots; i++) {
      double current_angle = i * angle_step + frame * 0.3;
      int dx = (int)(radius * Cos(current_angle));
      int dy = (int)(radius * Sin(current_angle));
      int dot_r = 3;
      DrawCircleFilled(gop, center_x + dx, center_y + dy, dot_r, white);
    }
    SyncBackBuffer(gop);
    gBS->Stall(50000);
  }

  gUseBackBuffer = FALSE;
  if (gBackBuffer) {
    gBS->FreePool(gBackBuffer);
    gBackBuffer = NULL;
  }
}

void CalcLoadAddressRange(Elf64_Ehdr* ehdr, UINT64* first, UINT64* last) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  *first = MAX_UINT64;
  *last = 0;
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    *first = MIN(*first, phdr[i].p_vaddr);
    *last = MAX(*last, phdr[i].p_vaddr + phdr[i].p_memsz);
  }
}

void CopyLoadSegments(Elf64_Ehdr* ehdr) {
  Elf64_Phdr* phdr = (Elf64_Phdr*)((UINT64)ehdr + ehdr->e_phoff);
  for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;

    UINT64 segm_in_file = (UINT64)ehdr + phdr[i].p_offset;
    CopyMem((VOID*)phdr[i].p_vaddr, (VOID*)segm_in_file, phdr[i].p_filesz);

    UINTN remain_bytes = phdr[i].p_memsz - phdr[i].p_filesz;
    SetMem((VOID*)(phdr[i].p_vaddr + phdr[i].p_filesz), remain_bytes, 0);
  }
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
  OpenGOP(image_handle, &gop);
  DrawDesktop(gop);

  Print(L"YinxuanLoader: Welcome to YinxuanLoader's World!\n");

  // Save Memory Map
  UINTN memmap_buf_size = 4096 * 16;
  VOID* memmap_buf = NULL;
  EFI_STATUS status;
  struct MemoryMap memmap;
  EFI_FILE_PROTOCOL* root_dir = NULL;

  while (1) {
    status = gBS->AllocatePool(EfiLoaderData, memmap_buf_size, &memmap_buf);
    if (EFI_ERROR(status)) {
      Print(L"Failed to allocate memory for memmap_buf\n");
      return status;
    }
    memmap.buffer_size = memmap_buf_size;
    memmap.buffer = memmap_buf;
    memmap.map_size = 0;
    memmap.map_key = 0;
    memmap.descriptor_size = 0;
    memmap.descriptor_version = 0;
    status = GetMemoryMap(&memmap);
    if (status == EFI_BUFFER_TOO_SMALL) {
      if (memmap_buf) gBS->FreePool(memmap_buf);
      memmap_buf_size *= 2;
      continue;
    } else if (EFI_ERROR(status)) {
      Print(L"GetMemoryMap failed: %r\n", status);
      if (memmap_buf) gBS->FreePool(memmap_buf);
      return status;
    }
    
    OpenRootDir(image_handle, &root_dir);
    EFI_FILE_PROTOCOL* memmap_file;
    root_dir->Open(root_dir, &memmap_file, L"\\memmap.csv", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    SaveMemoryMap(&memmap, memmap_file);
    memmap_file->Close(memmap_file);
    break;
  }
  Print(L"YinxuanLoader: Memory map saved successfully.\n");

  // Read Kernel
  EFI_FILE_PROTOCOL* kernel_file;
  status = root_dir->Open(root_dir, &kernel_file, L"\\kernel.elf", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) {
    Print(L"Failed to open kernel.elf: %r\n", status);
    while (1);
  }

  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12];
  status = kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid, &file_info_size, file_info_buffer);
  if (EFI_ERROR(status)) {
    Print(L"Failed to get kernel info: %r\n", status);
    while (1);
  }

  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;

  VOID* kernel_buffer;
  status = gBS->AllocatePool(EfiLoaderData, kernel_file_size, &kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"Failed to allocate pool for kernel: %r\n", status);
    while (1);
  }
  status = kernel_file->Read(kernel_file, &kernel_file_size, kernel_buffer);
  if (EFI_ERROR(status)) {
    Print(L"Failed to read kernel file: %r\n", status);
    while (1);
  }
  kernel_file->Close(kernel_file);

  // Load Kernel Segments
  Elf64_Ehdr* ehdr = (Elf64_Ehdr*)kernel_buffer;
  UINT64 kernel_first_addr, kernel_last_addr;
  CalcLoadAddressRange(ehdr, &kernel_first_addr, &kernel_last_addr);

  UINTN num_pages = (kernel_last_addr - kernel_first_addr + 0xfff) / 0x1000;
  status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, num_pages, &kernel_first_addr);
  if (EFI_ERROR(status)) {
    Print(L"Failed to allocate pages for kernel: %r\n", status);
    while (1);
  }

  CopyLoadSegments(ehdr);
  Print(L"Kernel Loaded at 0x%0lx (%lu bytes)\n", kernel_first_addr, kernel_last_addr - kernel_first_addr);

  UINT64 entry_addr = ehdr->e_entry;
  gBS->FreePool(kernel_buffer);

  // Prepare FrameBufferConfig
  struct FrameBufferConfig config = {
    (uint8_t*)gop->Mode->FrameBufferBase,
    gop->Mode->Info->PixelsPerScanLine,
    gop->Mode->Info->HorizontalResolution,
    gop->Mode->Info->VerticalResolution,
    0
  };
  switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
      config.pixel_format = kPixelRGBResv8BitPerColor;
      break;
    case PixelBlueGreenRedReserved8BitPerColor:
      config.pixel_format = kPixelBGRResv8BitPerColor;
      break;
    default:
      Print(L"Unimplemented pixel format: %d\n", gop->Mode->Info->PixelFormat);
      while (1);
  }

  // Exit Boot Services
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"Failed to get memory map for retry: %r\n", status);
      while (1);
    }
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      while (1);
    }
  }

  // Call Kernel
  typedef void EntryPointType(const struct FrameBufferConfig*);
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point(&config);

  while (1);
  return EFI_SUCCESS;
}
