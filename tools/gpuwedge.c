/* gpuwedge — reproduce B-39 trigger 1 (#56) with no compositor and no KMS.
 *
 * WHY THIS EXISTS
 *
 * The wedge trigger was narrowed to "a new client surface arriving while the
 * GPU is already rendering". Every observation so far was made through sway,
 * Xwayland, mediatek-drm and the M4U, so the trigger's own description could
 * not be tested without all of them. This does exactly that description and
 * nothing else: N processes on /dev/dri/renderD128, surfaceless EGL, no
 * card0, no DRM master, no compositor.
 *
 * Two consequences beyond isolation:
 *
 *  1. With no DRM master held the panel stays a text console, so a kernel
 *     oops or panic RENDERS TO THE GLASS and gemini-eyes.py can photograph
 *     it. On a rig with no serial adapter that is the only path to a stack
 *     trace. Set kernel.panic_on_oops=1 / panic_on_warn=1 / panic=0 first.
 *  2. It is scriptable, so a bisect can run unattended.
 *
 * MODES
 *
 *   hog   — one long-lived context submitting back-to-back jobs forever.
 *           This is the "GPU already rendering" half.
 *   poke  — repeatedly create a fresh EGL context + gbm BO, draw once,
 *           tear it down. This is the "new client surface arriving" half.
 *           Each iteration is a brand-new GPU address space as far as
 *           panfrost's MMU is concerned, which is the suspected mechanism:
 *           panfrost_mmu_as_get() evicts an LRU address space when the slots
 *           run out, and it does that while the evicted context has jobs in
 *           flight.
 *   hold  — create N contexts and KEEP them, to exhaust the MMU address
 *           space slots without any teardown churn. Distinguishes "eviction
 *           under load" from "context creation under load".
 *   run   — fork one hog and one poke and run the pair (the actual repro).
 *
 * Every phase writes a marker to /dev/kmsg so the netconsole log and the
 * photograph share a timeline.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

static const char *NODE = "/dev/dri/renderD128";

/* Fragment shader with a fixed-count loop so one draw costs real GPU time.
 * GLES2 requires a constant loop bound; ITER is patched in as a literal. */
static const char *vs_src =
    "attribute vec2 p;\n"
    "varying vec2 uv;\n"
    "void main(){ uv = p; gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *fs_fmt =
    "precision highp float;\n"
    "varying vec2 uv;\n"
    "void main(){\n"
    "  vec2 z = uv * 1.7;\n"
    "  float a = 0.0;\n"
    "  for (int i = 0; i < %d; i++) {\n"
    "    z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + uv;\n"
    "    a += dot(z, z) * 0.001;\n"
    "  }\n"
    "  gl_FragColor = vec4(fract(a), fract(a*0.5), 0.25, 1.0);\n"
    "}\n";

static void kmsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[gpuwedge] %s\n", buf);
    fflush(stderr);
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "gpuwedge: %s\n", buf);
        close(fd);
    }
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct ctx {
    int fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog, rb, fbo;
    int size;
};

static GLuint mkshader(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        exit(1);
    }
    return s;
}

/* Bring up a complete, independent GPU client: its own fd, its own gbm
 * device, its own EGLDisplay and context. Separate fds matter — panfrost
 * keys an address space off the DRM file, so two contexts on one fd share
 * one AS and would not exercise the path under suspicion. */
static int ctx_open(struct ctx *c, int size, int iter)
{
    memset(c, 0, sizeof *c);
    c->size = size;

    c->fd = open(NODE, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) { perror("open renderD128"); return -1; }

    c->gbm = gbm_create_device(c->fd);
    if (!c->gbm) { fprintf(stderr, "gbm_create_device failed\n"); return -1; }

    c->dpy = eglGetDisplay((EGLNativeDisplayType)c->gbm);
    if (c->dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return -1; }
    if (!eglInitialize(c->dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfgattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                         EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(c->dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return -1;
    }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    c->ctx = eglCreateContext(c->dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (c->ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return -1; }
    if (!eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c->ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n"); return -1;
    }

    glGenRenderbuffers(1, &c->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, c->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, size, size);
    glGenFramebuffers(1, &c->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, c->rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete\n"); return -1;
    }

    char fs_src[2048];
    snprintf(fs_src, sizeof fs_src, fs_fmt, iter);
    c->prog = glCreateProgram();
    glAttachShader(c->prog, mkshader(GL_VERTEX_SHADER, vs_src));
    glAttachShader(c->prog, mkshader(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(c->prog, 0, "p");
    glLinkProgram(c->prog);
    GLint ok = 0;
    glGetProgramiv(c->prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); return -1; }
    glUseProgram(c->prog);
    glViewport(0, 0, size, size);
    return 0;
}

static void ctx_close(struct ctx *c)
{
    eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(c->dpy, c->ctx);
    eglTerminate(c->dpy);
    gbm_device_destroy(c->gbm);
    close(c->fd);
}

/* One full-viewport quad. No glFinish: the point is to leave work queued. */
static void ctx_draw(struct ctx *c)
{
    static const GLfloat quad[] = { -1.f, -1.f,  1.f, -1.f, -1.f, 1.f,
                                     1.f, -1.f,  1.f,  1.f, -1.f, 1.f };
    glClearColor(0.1f, 0.f, 0.2f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFlush();
}

static volatile sig_atomic_t stop_flag;
static void on_alarm(int sig) { (void)sig; stop_flag = 1; }

static int mode_hog(int seconds, int size, int iter, int batch)
{
    struct ctx c;
    if (ctx_open(&c, size, iter) < 0) return 1;
    kmsg("HOG up: %dx%d iter=%d batch=%d renderer=%s",
         size, size, iter, batch, (const char *)glGetString(GL_RENDERER));

    signal(SIGALRM, on_alarm);
    if (seconds > 0) alarm(seconds);

    /* Time one batch first, so the log records what "busy" costs here. If a
     * batch already exceeds panfrost's 500 ms JOB_TIMEOUT_MS the hog is
     * self-faulting and the experiment is confounded — say so loudly. */
    double t0 = now();
    for (int i = 0; i < batch; i++) ctx_draw(&c);
    glFinish();
    double dt = now() - t0;
    kmsg("HOG batch of %d draws = %.1f ms%s", batch, dt * 1e3,
         dt > 0.45 ? "  *** AT OR OVER THE 500 ms JOB TIMEOUT ***" : "");

    unsigned long batches = 0;
    while (!stop_flag) {
        for (int i = 0; i < batch; i++) ctx_draw(&c);
        glFinish();
        batches++;
    }
    kmsg("HOG done after %lu batches", batches);
    ctx_close(&c);
    return 0;
}

static int mode_poke(int count, int interval_ms, int size, int iter)
{
    kmsg("POKE start: %d contexts, %d ms apart, %dx%d iter=%d",
         count, interval_ms, size, size, iter);
    for (int i = 0; i < count || count == 0; i++) {
        struct ctx c;
        kmsg("POKE %d: creating context", i);
        if (ctx_open(&c, size, iter) < 0) {
            kmsg("POKE %d: context creation FAILED (errno %d)", i, errno);
            return 1;
        }
        ctx_draw(&c);
        glFinish();
        kmsg("POKE %d: drew and finished, tearing down", i);
        ctx_close(&c);
        if (interval_ms) usleep(interval_ms * 1000);
    }
    kmsg("POKE done");
    return 0;
}

/* Hold N simultaneous contexts. Midgard has a small, fixed number of MMU
 * address spaces; once they are all in use panfrost_mmu_as_get() starts
 * evicting. Holding without churn separates "many address spaces exist"
 * from "address spaces are being recycled under load". */
static int mode_hold(int count, int seconds, int size, int iter)
{
    struct ctx *cs = calloc(count, sizeof *cs);
    if (!cs) return 1;
    kmsg("HOLD start: opening %d simultaneous contexts", count);
    int opened = 0;
    for (int i = 0; i < count; i++) {
        if (ctx_open(&cs[i], size, iter) < 0) {
            kmsg("HOLD: context %d failed to open, stopping there", i);
            break;
        }
        opened++;
        ctx_draw(&cs[i]);
        glFinish();
        kmsg("HOLD: context %d up and drawn", i);
    }
    kmsg("HOLD: %d contexts live; drawing round-robin for %d s", opened, seconds);
    signal(SIGALRM, on_alarm);
    if (seconds > 0) alarm(seconds);
    unsigned long rounds = 0;
    while (!stop_flag) {
        for (int i = 0; i < opened; i++) {
            eglMakeCurrent(cs[i].dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, cs[i].ctx);
            ctx_draw(&cs[i]);
        }
        for (int i = 0; i < opened; i++) {
            eglMakeCurrent(cs[i].dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, cs[i].ctx);
            glFinish();
        }
        rounds++;
    }
    kmsg("HOLD done after %lu rounds", rounds);
    for (int i = 0; i < opened; i++) ctx_close(&cs[i]);
    free(cs);
    return 0;
}

/* Like hog, but with idle gaps longer than panfrost's 50 ms autosuspend
 * delay, so the MFG power domain is cycled between every burst. A steady hog
 * never suspends the GPU at all, which means it cannot exercise the
 * suspend/resume path — and on this SoC that path carries bespoke VGPU_SRAM
 * LDO handling (pmdomain/0007) that nothing else touches. A compositor
 * driving a 60 Hz desktop is bursty; `hog` is not. */
static int mode_burst(int seconds, int on_ms, int off_ms, int size, int iter)
{
    struct ctx c;
    if (ctx_open(&c, size, iter) < 0) return 1;
    kmsg("BURST up: %d ms on / %d ms off (autosuspend is 50 ms), %dx%d iter=%d",
         on_ms, off_ms, size, size, iter);

    signal(SIGALRM, on_alarm);
    if (seconds > 0) alarm(seconds);

    unsigned long bursts = 0;
    while (!stop_flag) {
        double until = now() + on_ms / 1000.0;
        while (now() < until) { ctx_draw(&c); glFinish(); }
        bursts++;
        usleep(off_ms * 1000);
    }
    kmsg("BURST done after %lu bursts", bursts);
    ctx_close(&c);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: gpuwedge hog   [seconds] [size] [iter] [batch]\n"
        "       gpuwedge burst [seconds] [on_ms] [off_ms] [size] [iter]\n"
        "       gpuwedge poke  [count] [interval_ms] [size] [iter]\n"
        "       gpuwedge hold  [count] [seconds] [size] [iter]\n"
        "       gpuwedge run   [seconds] [poke_interval_ms]\n"
        "                      fork a steady hog, then poke it\n"
        "       gpuwedge runburst [seconds] [poke_interval_ms] [on_ms] [off_ms]\n"
        "                      fork a BURSTY hog, then poke it — the shape a\n"
        "                      60 Hz compositor actually has\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { usage(); return 2; }

    if (!strcmp(argv[1], "hog")) {
        return mode_hog(argc > 2 ? atoi(argv[2]) : 60,
                        argc > 3 ? atoi(argv[3]) : 512,
                        argc > 4 ? atoi(argv[4]) : 24,
                        argc > 5 ? atoi(argv[5]) : 8);
    }
    if (!strcmp(argv[1], "poke")) {
        return mode_poke(argc > 2 ? atoi(argv[2]) : 40,
                         argc > 3 ? atoi(argv[3]) : 250,
                         argc > 4 ? atoi(argv[4]) : 64,
                         argc > 5 ? atoi(argv[5]) : 4);
    }
    if (!strcmp(argv[1], "burst")) {
        return mode_burst(argc > 2 ? atoi(argv[2]) : 60,
                          argc > 3 ? atoi(argv[3]) : 60,
                          argc > 4 ? atoi(argv[4]) : 120,
                          argc > 5 ? atoi(argv[5]) : 512,
                          argc > 6 ? atoi(argv[6]) : 24);
    }
    if (!strcmp(argv[1], "hold")) {
        return mode_hold(argc > 2 ? atoi(argv[2]) : 24,
                         argc > 3 ? atoi(argv[3]) : 30,
                         argc > 4 ? atoi(argv[4]) : 128,
                         argc > 5 ? atoi(argv[5]) : 8);
    }
    if (!strcmp(argv[1], "run")) {
        int seconds = argc > 2 ? atoi(argv[2]) : 90;
        int interval = argc > 3 ? atoi(argv[3]) : 250;

        kmsg("RUN begin: hog for %d s, poke every %d ms", seconds, interval);
        pid_t hog = fork();
        if (hog == 0) {
            _exit(mode_hog(seconds, 512, 24, 8));
        }
        /* Let the hog actually get onto the GPU before the first new client
         * arrives. The whole trigger is "while it is already rendering". */
        sleep(3);
        pid_t poke = fork();
        if (poke == 0) {
            _exit(mode_poke(0, interval, 64, 4));
        }
        int st;
        waitpid(hog, &st, 0);
        kill(poke, SIGKILL);
        waitpid(poke, NULL, 0);
        kmsg("RUN survived: hog exited %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        printf("\nRESULT: SURVIVED %d s of hog + poke\n", seconds);
        return 0;
    }
    if (!strcmp(argv[1], "runburst")) {
        int seconds  = argc > 2 ? atoi(argv[2]) : 90;
        int interval = argc > 3 ? atoi(argv[3]) : 130;
        int on_ms    = argc > 4 ? atoi(argv[4]) : 60;
        int off_ms   = argc > 5 ? atoi(argv[5]) : 120;

        kmsg("RUNBURST begin: %d s, burst %d/%d ms, poke every %d ms",
             seconds, on_ms, off_ms, interval);
        pid_t hog = fork();
        if (hog == 0) _exit(mode_burst(seconds, on_ms, off_ms, 512, 24));
        sleep(3);
        pid_t poke = fork();
        if (poke == 0) _exit(mode_poke(0, interval, 64, 4));
        int st;
        waitpid(hog, &st, 0);
        kill(poke, SIGKILL);
        waitpid(poke, NULL, 0);
        kmsg("RUNBURST survived");
        printf("\nRESULT: SURVIVED %d s of bursty hog + poke\n", seconds);
        return 0;
    }
    usage();
    return 2;
}
