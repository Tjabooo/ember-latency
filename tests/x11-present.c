// Regression: DXGI owns the timing queue and selects an unlocked present mode
// on a swapchain created as FIFO. No app present ID may be invented or changed.
#include "../src/ember-latency-layer.c"
#include <assert.h>

static void *instance_key = (void *)1, *device_key = (void *)2;
static unsigned submits, presents, destroyed;
static const void *expected_chain;

static VkResult VKAPI_CALL create_surface(VkInstance i, const void *ci,
        const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{
    (void)i; (void)ci; (void)a;
    *out = (VkSurfaceKHR)(uintptr_t)123;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_surface(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a)
{
    (void)i; (void)s; (void)a;
    destroyed++;
}
static PFN_vkVoidFunction VKAPI_CALL get_proc(VkInstance i, const char *name)
{
    (void)i;
    if (!strcmp(name, "vkCreateXcbSurfaceKHR") || !strcmp(name, "vkCreateXlibSurfaceKHR"))
        return (PFN_vkVoidFunction)create_surface;
    if (!strcmp(name, "vkDestroySurfaceKHR")) return (PFN_vkVoidFunction)destroy_surface;
    return NULL;
}
static VkResult VKAPI_CALL create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci,
        const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
    (void)d; (void)a;
    // The application's timing ownership must survive unchanged.
    assert(ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT);
    *out = (VkSwapchainKHR)(uintptr_t)456;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL create_semaphore(VkDevice d, const VkSemaphoreCreateInfo *ci,
        const VkAllocationCallbacks *a, VkSemaphore *out)
{
    (void)d; (void)ci; (void)a; *out = (VkSemaphore)(uintptr_t)789; return VK_SUCCESS;
}
static VkResult VKAPI_CALL create_fence(VkDevice d, const VkFenceCreateInfo *ci,
        const VkAllocationCallbacks *a, VkFence *out)
{
    (void)d; (void)ci; (void)a; *out = (VkFence)(uintptr_t)987; return VK_SUCCESS;
}
static VkResult VKAPI_CALL submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    (void)q; (void)f;
    assert(n == 1 && si->commandBufferCount == 0 && si->waitSemaphoreCount == 1);
    submits++;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL present(VkQueue q, const VkPresentInfoKHR *pi)
{
    (void)q;
    assert(pi->pNext == expected_chain); // Preserve the application's modes/timing/IDs.
    presents++;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL wait2(VkDevice d, VkSwapchainKHR s, const VkPresentWait2InfoKHR *wi)
{
    (void)d; (void)s; (void)wi; return VK_SUCCESS;
}

int main(void)
{
    VkInstance instance = (VkInstance)&instance_key;
    VkDevice device = (VkDevice)&device_key;
    VkQueue queue = (VkQueue)&device_key;
    inst_t in = { .key = instance_key, .inst = instance, .gipa = get_proc };
    ldev_t dev = { .key = device_key, .dev = device, .instance = &in, .active = true,
        .create_sc = create_swapchain, .present = present, .wait2 = wait2,
        .queue_submit = submit, .create_sem = create_semaphore, .create_fence = create_fence,
        .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
    g_insts = &in;
    g_devs = &dev;
    VkSurfaceKHR surface;
    assert(el_GetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR") == (PFN_vkVoidFunction)el_CreateXcbSurfaceKHR);
    assert(el_CreateXcbSurfaceKHR(instance, NULL, NULL, &surface) == VK_SUCCESS);
    assert(is_x11_surface(&in, surface));
    assert(!is_x11_surface(&in, (VkSurfaceKHR)(uintptr_t)124));
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .flags = VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
                 VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR };
    VkSwapchainKHR sc;
    assert(el_CreateSwapchainKHR(device, &ci, NULL, &sc) == VK_SUCCESS);
    assert(dev.scs->x11 && dev.scs->mode == SC_V2 && !dev.scs->timing);

    VkPresentModeKHR mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    VkSwapchainPresentModeInfoKHR modes = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
        .swapchainCount = 1, .pPresentModes = &mode };
    VkSemaphore semaphore = (VkSemaphore)(uintptr_t)333;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &modes,
        .swapchainCount = 1, .pSwapchains = &sc, .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore };
    expected_chain = &modes;
    dev.last_ret = now_ns();
    push_press(dev.last_ret - 1000000);
    assert(el_QueuePresentKHR(queue, &pi) == VK_SUCCESS);
    assert(submits == 1 && presents == 1 && dev.jobs && dev.jobs->fence && dev.jobs->n == 1);
    free(dev.jobs); dev.jobs = dev.jobs_tail = NULL;
    assert(!dev.scs->app_ids && dev.scs->next_id == 0);
    // No presses: no submission or job allocation on the ordinary frame path.
    assert(el_QueuePresentKHR(queue, &pi) == VK_SUCCESS);
    assert(submits == 1 && presents == 2 && !dev.jobs);

    // FIFO + an application-owned v2 ID uses its ID without changing the chain.
    uint64_t id = 42;
    VkPresentId2KHR app_id = { .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .pNext = &modes, .swapchainCount = 1, .pPresentIds = &id };
    mode = VK_PRESENT_MODE_FIFO_KHR;
    pi.pNext = &app_id; expected_chain = &app_id;
    push_press(dev.last_ret - 1000000);
    assert(el_QueuePresentKHR(queue, &pi) == VK_SUCCESS);
    assert(submits == 1 && dev.jobs && !dev.jobs->fence && dev.jobs->id == id && id == 42);
    free(dev.jobs); free(dev.scs);
    el_DestroySurfaceKHR(instance, surface, NULL);
    assert(destroyed == 1 && !is_x11_surface(&in, surface));
    assert(el_CreateXlibSurfaceKHR(instance, NULL, NULL, &surface) == VK_SUCCESS);
    assert(is_x11_surface(&in, surface));
    el_DestroySurfaceKHR(instance, surface, NULL);
    puts("X11 surface, DXGI present-mode, v2 ID preservation and idle fast path: passed");
    return 0;
}
