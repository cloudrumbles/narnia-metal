/*
 * Presenter module for the Linux test host: copies every presented frame
 * into a host-visible buffer, prints the frame rate, and writes some frames
 * as PNGs.
 *
 *   NM_DUMP_DIR=dir      write PNGs there (default: don't write)
 *   NM_DUMP_EVERY=n      write every nth frame (default 60), plus frame 0
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_presenter_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

struct png_sink {
   VkDevice device;
   PFN_vkDestroyBuffer DestroyBuffer;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;

   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;

   uint32_t swapchain_id;
   uint32_t width, height;
   VkFormat format;

   const char *dir;
   uint64_t every;

   uint64_t frames;
   double last_report;
   uint64_t last_report_frames;
};

static double
now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
   p[0] = v >> 24;
   p[1] = v >> 16;
   p[2] = v >> 8;
   p[3] = v;
}

static void
png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
   uint8_t hdr[8];
   put_be32(hdr, len);
   memcpy(hdr + 4, type, 4);
   fwrite(hdr, 1, 8, f);
   if (len)
      fwrite(data, 1, len, f);
   uLong crc = crc32(0, (const Bytef *)type, 4);
   if (len)
      crc = crc32(crc, data, len);
   uint8_t c[4];
   put_be32(c, crc);
   fwrite(c, 1, 4, f);
}

static void
write_png(const char *path, const uint8_t *bgra_or_rgba, uint32_t w,
          uint32_t h, bool bgra)
{
   const size_t row = (size_t)w * 3 + 1;
   uint8_t *raw = malloc(row * h);
   if (!raw)
      return;
   for (uint32_t y = 0; y < h; y++) {
      uint8_t *dst = raw + row * y;
      const uint8_t *src = bgra_or_rgba + (size_t)w * 4 * y;
      *dst++ = 0; /* filter: none */
      for (uint32_t x = 0; x < w; x++, src += 4) {
         *dst++ = bgra ? src[2] : src[0];
         *dst++ = src[1];
         *dst++ = bgra ? src[0] : src[2];
      }
   }

   uLongf zlen = compressBound(row * h);
   uint8_t *z = malloc(zlen);
   if (!z || compress2(z, &zlen, raw, row * h, 6) != Z_OK) {
      free(raw);
      free(z);
      return;
   }
   free(raw);

   FILE *f = fopen(path, "wb");
   if (f) {
      static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a,
                                      '\n' };
      fwrite(sig, 1, 8, f);
      uint8_t ihdr[13];
      put_be32(ihdr, w);
      put_be32(ihdr + 4, h);
      ihdr[8] = 8;  /* bit depth */
      ihdr[9] = 2;  /* truecolor */
      ihdr[10] = 0; /* deflate */
      ihdr[11] = 0; /* adaptive filtering */
      ihdr[12] = 0; /* no interlace */
      png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
      png_chunk(f, "IDAT", z, zlen);
      png_chunk(f, "IEND", NULL, 0);
      fclose(f);
   }
   free(z);
}

static void *
png_create(void *user, const struct nm_presenter_device *dev,
           const struct nm_presenter_frame *first)
{
   struct png_sink *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;

   PFN_vkGetDeviceProcAddr gdpa = dev->get_device_proc_addr;
   VkDevice device = dev->device;
   s->device = device;
   s->DestroyBuffer = (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
   s->FreeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
   s->CmdCopyImageToBuffer =
      (PFN_vkCmdCopyImageToBuffer)gdpa(device, "vkCmdCopyImageToBuffer");
   s->CmdPipelineBarrier =
      (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier");
   PFN_vkCreateBuffer CreateBuffer =
      (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
   PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements =
      (PFN_vkGetBufferMemoryRequirements)gdpa(
         device, "vkGetBufferMemoryRequirements");
   PFN_vkAllocateMemory AllocateMemory =
      (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
   PFN_vkBindBufferMemory BindBufferMemory =
      (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
   PFN_vkMapMemory MapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
   PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)dev->get_instance_proc_addr(
         dev->instance, "vkGetPhysicalDeviceMemoryProperties");

   s->swapchain_id = first->swapchain_id;
   s->width = first->width;
   s->height = first->height;
   s->format = first->format;
   s->dir = getenv("NM_DUMP_DIR");
   const char *every = getenv("NM_DUMP_EVERY");
   s->every = every && atoi(every) > 0 ? atoi(every) : 60;
   s->last_report = now_s();

   const VkBufferCreateInfo buf_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = (VkDeviceSize)s->width * s->height * 4,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   if (CreateBuffer(device, &buf_info, NULL, &s->buffer) != VK_SUCCESS)
      goto fail;

   VkMemoryRequirements reqs;
   GetBufferMemoryRequirements(device, s->buffer, &reqs);
   VkPhysicalDeviceMemoryProperties props;
   GetMemoryProperties(dev->physical_device, &props);
   uint32_t type = UINT32_MAX;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (props.memoryTypes[i].propertyFlags & want) == want) {
         type = i;
         break;
      }
   }
   if (type == UINT32_MAX)
      goto fail;

   const VkMemoryAllocateInfo mem_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = reqs.size,
      .memoryTypeIndex = type,
   };
   if (AllocateMemory(device, &mem_info, NULL, &s->memory) != VK_SUCCESS ||
       BindBufferMemory(device, s->buffer, s->memory, 0) != VK_SUCCESS ||
       MapMemory(device, s->memory, 0, VK_WHOLE_SIZE, 0, &s->map) !=
          VK_SUCCESS)
      goto fail;

   fprintf(stderr, "nm-host: swapchain %u: %ux%u format %d%s%s\n",
           s->swapchain_id, s->width, s->height, s->format,
           s->dir ? ", dumping to " : "", s->dir ? s->dir : "");
   return s;

fail:
   fprintf(stderr, "nm-host: png presenter: buffer setup failed\n");
   if (s->buffer)
      s->DestroyBuffer(device, s->buffer, NULL);
   if (s->memory)
      s->FreeMemory(device, s->memory, NULL);
   free(s);
   return NULL;
}

static void
png_record(void *sink, VkCommandBuffer cmd, VkImage src,
           const struct nm_presenter_frame *frame)
{
   struct png_sink *s = sink;
   const VkBufferImageCopy region = {
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .layerCount = 1,
      },
      .imageExtent = { s->width, s->height, 1 },
   };
   s->CmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->buffer, 1, &region);

   const VkBufferMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = s->buffer,
      .size = VK_WHOLE_SIZE,
   };
   s->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier,
                         0, NULL);
}

static void
png_complete(void *sink, const struct nm_presenter_frame *frame)
{
   struct png_sink *s = sink;
   s->frames++;

   const double t = now_s();
   if (t - s->last_report >= 1.0) {
      fprintf(stderr, "nm-host: swapchain %u: %.1f fps (%llu frames)\n",
              s->swapchain_id,
              (s->frames - s->last_report_frames) / (t - s->last_report),
              (unsigned long long)s->frames);
      s->last_report = t;
      s->last_report_frames = s->frames;
   }

   if (s->dir && (frame->frame == 0 || frame->frame % s->every == 0)) {
      char path[512];
      snprintf(path, sizeof(path), "%s/sc%u-%06llu.png", s->dir,
               s->swapchain_id, (unsigned long long)frame->frame);
      const bool bgra = s->format == VK_FORMAT_B8G8R8A8_UNORM ||
                        s->format == VK_FORMAT_B8G8R8A8_SRGB;
      write_png(path, s->map, s->width, s->height, bgra);
   }
}

static void
png_destroy(void *sink)
{
   struct png_sink *s = sink;
   fprintf(stderr, "nm-host: swapchain %u: destroyed after %llu frames\n",
           s->swapchain_id, (unsigned long long)s->frames);
   s->DestroyBuffer(s->device, s->buffer, NULL);
   s->FreeMemory(s->device, s->memory, NULL);
   free(s);
}

const struct nm_presenter_ops nm_presenter_png = {
   .create = png_create,
   .record = png_record,
   .complete = png_complete,
   .destroy = png_destroy,
};
