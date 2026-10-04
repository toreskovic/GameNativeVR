/* Production bridge test: stereo copies must precede the producer fence, and
 * output reuse must wait for Android release. No Vulkan driver is invoked. */
#include <pthread.h>
#include <errno.h>
static int no_worker(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p)
{ (void)t; (void)a; (void)f; (void)p; return EAGAIN; }
#define pthread_create no_worker
#include "../../app/src/main/windows/openxr_runtime/unix/gamenative_openxr_unix.c"
#undef pthread_create
#include <assert.h>
#include <sys/eventfd.h>
static atomic_int deliver_fence = -1;
static unsigned acquire_requests[2];
static unsigned resets, copies, waits, commands, barrier_calls, destroy_count;
static int fail_submit, fail_export, already_complete, fail_create;
static unsigned completion_creates, completion_destroys, completion_waits;
static int signal_fd = -1;
static atomic_uint early_frames;
static VkResult make_completion(VkDevice d, VkSemaphore *s) {
 (void)d; ++completion_creates;
 if (fail_create) return VK_ERROR_EXTENSION_NOT_PRESENT;
 *s=(VkSemaphore)8; signal_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK); assert(signal_fd>=0);
 completion_waits=waits; return VK_SUCCESS;
}
static VkResult export_signal(VkDevice d, VkSemaphore s, int *fd) {
 (void)d; assert(s==(VkSemaphore)8 && waits==completion_waits);
 if(fail_export) return VK_ERROR_TOO_MANY_OBJECTS;
 *fd=already_complete ? -1 : dup(signal_fd); return VK_SUCCESS;
}
static void destroy_signal(VkDevice d, VkSemaphore s) {
 (void)d; assert(s==(VkSemaphore)8 && (fail_submit || waits>completion_waits));
 close(signal_fd); signal_fd=-1; ++completion_destroys;
}
static VkResult make_fence(VkDevice d, const VkFenceCreateInfo *i, const VkAllocationCallbacks *a, VkFence *f)
{ (void)d; (void)i; (void)a; *f=(VkFence)3; return VK_SUCCESS; }
static void destroy_fence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{ (void)d; (void)f; (void)a; ++destroy_count; }
static VkResult wait_fence(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t ns)
{ (void)d; (void)f; (void)all; (void)ns; assert(n==1); ++waits;
 if(signal_fd>=0) { uint64_t one=1; assert(write(signal_fd,&one,sizeof(one))==sizeof(one)); }
 return VK_SUCCESS; }
static VkResult reset_command(VkCommandBuffer c, VkCommandBufferResetFlags f)
{ (void)c; assert(!f); ++resets; return VK_SUCCESS; }
static VkResult begin_command(VkCommandBuffer c, const VkCommandBufferBeginInfo *i)
{ (void)c; assert(!i->flags); return VK_SUCCESS; }
static VkResult end_command(VkCommandBuffer c) { (void)c; return VK_SUCCESS; }
static void copy_image(VkCommandBuffer c, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *p)
{
 (void)src; (void)sl; (void)dst; (void)dl;
 assert(n==1 && p->srcSubresource.baseArrayLayer == (uint32_t)((uintptr_t)c-10));
 assert(p->dstSubresource.baseArrayLayer==0 && p->extent.width==64 && p->extent.height==64);
 ++copies;
}
static void barriers(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags f,
 uint32_t m, const VkMemoryBarrier *mb, uint32_t b, const VkBufferMemoryBarrier *bb, uint32_t n, const VkImageMemoryBarrier *ib)
{
 (void)c; (void)src; (void)dst; (void)f; (void)m; (void)mb; (void)b; (void)bb;
 assert(n==2); ++barrier_calls;
 if(ib[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) assert(ib[0].srcAccessMask==VK_ACCESS_MEMORY_WRITE_BIT);
 if (ib[1].newLayout==VK_IMAGE_LAYOUT_GENERAL) {
  assert(ib[1].srcQueueFamilyIndex==3 && ib[1].dstQueueFamilyIndex==VK_QUEUE_FAMILY_FOREIGN_EXT);
  assert(ib[1].dstAccessMask==0);
 } else if (ib[1].oldLayout==VK_IMAGE_LAYOUT_GENERAL) {
  assert(ib[1].srcQueueFamilyIndex==VK_QUEUE_FAMILY_FOREIGN_EXT && ib[1].dstQueueFamilyIndex==3);
  assert(ib[1].srcAccessMask==0);
 }
}
static VkResult queue_submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
 (void)q; assert(n==1 && f==(VkFence)3); commands=si->commandBufferCount;
 assert(si->signalSemaphoreCount==(signal_fd>=0 ? 1u : 0u));
 if(si->signalSemaphoreCount) assert(si->pSignalSemaphores[0]==(VkSemaphore)8);
 assert(commands==0 || (commands==2 && si->pCommandBuffers[0]==(VkCommandBuffer)10 && si->pCommandBuffers[1]==(VkCommandBuffer)11));
 return fail_submit ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
static VkResult unused_properties(VkDevice d, const AHardwareBuffer *b, VkAndroidHardwareBufferPropertiesANDROID *p)
{ (void)d; (void)b; (void)p; assert(0); return VK_ERROR_UNKNOWN; }
static void *server(void *opaque)
{
 int fd=*(int *)opaque; char line[512];
 while(read_line(fd,line,sizeof(line))) {
  if(!strcmp(line,"STOP")) break;
  if(!strncmp(line,"FRAME ",6)) {
   if(strstr(line,"fence=1")) {
    assert(write_all(fd,"OK\n",3)); int acquire=recv_fd(fd); assert(acquire>=0);
    struct pollfd p={acquire,POLLIN,0}; assert(poll(&p,1,0)==0);
    assert(waits==completion_waits); atomic_fetch_add(&early_frames,1); close(acquire);
   } else assert(waits);
  }
  else {
   assert(!strncmp(line,"ACQUIRE ",8));
   unsigned eye = strstr(line,"eye=1") ? 1 : 0;
   ++acquire_requests[eye];
   int fence = atomic_exchange(&deliver_fence, -1);
   if (fence >= 0) {
    assert(write_all(fd,"OK fence=1\n",11) && send_fd(fd, fence));
    continue;
   }
  }
  assert(write_all(fd,"OK fence=0\n",11));
 }
 return NULL;
}
int main(void)
{
 p_vkCreateFence=make_fence; p_vkDestroyFence=destroy_fence; p_vkWaitForFences=wait_fence;
 p_vkResetCommandBuffer=reset_command; p_vkBeginCommandBuffer=begin_command; p_vkEndCommandBuffer=end_command;
 p_vkCmdCopyImage=copy_image; p_vkCmdPipelineBarrier=barriers; p_vkQueueSubmit=queue_submit;
 p_vkGetAndroidHardwareBufferPropertiesANDROID=unused_properties;
 queue_family_index=3; command_pool=(VkCommandPool)1;
 struct gn_swapchain *sc=&swapchains[0]; sc->width=sc->height=64; sc->array_size=2; sc->image_count=1;
 struct gn_image *im=&sc->images[0]; im->image=(VkImage)4; im->registered_eye_mask=3;
 for(unsigned i=0;i<2;i++) {
  im->pending_release_fd[i]=-1; im->registered_array_index[i]=i; im->transport_kind[i]=GN_TRANSPORT_AHARDWAREBUFFER;
  im->transport[i].image=(VkImage)(uintptr_t)(5+i); im->transport[i].hardware_buffer=(void *)1;
  im->transport[i].command_buffer=(VkCommandBuffer)(uintptr_t)(10+i); im->transport[i].registered=1;
 }
 int sockets[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets)); transport_fd=sockets[0];
 pthread_t thread; assert(!pthread_create(&thread,NULL,server,&sockets[1]));
 struct gn_unix_submit_view_args views[2]={{0}}; views[1].eye=1; views[1].array_index=1;
 assert(submit_views_async(views,2));
 assert(commands==2 && resets==2 && copies==2 && barrier_calls==4 && waits==1);
 assert(im->transport[0].external && im->transport[1].external && outputs_handed_off==2);
 /* Re-present without reacquiring: no overwriting Android's immutable image. */
 assert(submit_views_async(views,2)); assert(commands==0 && resets==2 && copies==2);
 struct gn_unix_acquire_image_args acquire={0};
 int release = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC); assert(release >= 0);
 atomic_store(&deliver_fence, release);
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_ERROR_TIMEOUT && outputs_handed_off==2);
 assert(im->pending_release_fd[0]>=0);
 uint64_t one=1; assert(write(release,&one,sizeof(one))==sizeof(one));
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS && outputs_handed_off==0);
 assert(acquire_requests[0]==1 && acquire_requests[1]==1 && im->pending_release_fd[0]==-1);
 close(release);
 assert(submit_views_async(views,2)); assert(commands==2 && resets==4 && copies==4);
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 assert(submit_views_async(views,2)); assert(commands==2 && resets==4 && copies==4); // cached steady commands
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 fail_submit=1; assert(!submit_views_async(views,2)); assert(!im->transport[0].external && outputs_handed_off==0);
 fail_submit=0;
 create_completion=make_completion; export_completion=export_signal; destroy_completion=destroy_signal;
 assert(submit_views_async(views,2));
 assert(early_frames==2 && completion_creates==1 && completion_destroys==1);
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 fail_export=1; assert(submit_views_async(views,2));
 assert(early_frames==2 && completion_destroys==2); // export failure waits before notification
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 fail_export=0; already_complete=1; assert(submit_views_async(views,2));
 assert(early_frames==2 && completion_destroys==3); // successful -1 is already complete
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 already_complete=0; fail_submit=1; assert(!submit_views_async(views,2));
 assert(completion_destroys==4 && !outputs_handed_off);
 fail_submit=0; fail_create=1; assert(submit_views_async(views,2));
 assert(completion_destroys==4 && early_frames==2);
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 fail_create=0;
 unsigned before=completion_creates; atomic_store(&unannounced_submits,1);
 assert(submit_views_async(views,2)); assert(completion_creates==before); // no overtaking fallback
 atomic_store(&unannounced_submits,0);
 unix_acquire_image(&acquire); assert(acquire.result==GN_UNIX_SUCCESS);
 assert(submit_views_async(views,2)); assert(early_frames==4);
 assert(write_all(transport_fd,"STOP\n",5)); pthread_join(thread,NULL);
 close_transport(); assert(transport_ownership_failed && !submit_views_async(views,2));
 close(sockets[1]);
 puts("Same-device AHB: stereo producer copies, FOREIGN ownership, immutable re-present, release timeout/retry, early per-eye FDs, separate retirement, export/create fallback, ordered notifications, cached commands, failed submit and disconnect passed");
 return 0;
}
