#include <pwd.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/utsname.h>
#include <notcurses/notcurses.h>

typedef struct distro_info {
  const char* name;            // must match 'lsb_release -i'
  const char* logofile;        // kept at original aspect ratio, lain atop bg
} distro_info;

typedef struct fetched_info {
  char* username;              // we borrow a reference
  char hostname[_POSIX_HOST_NAME_MAX];
  const distro_info* distro;
  char* distro_release;
  char* kernel;                // strdup(uname(2)->name)
  char* kernver;               // strdup(uname(2)->version);
} fetched_info;

static distro_info distros[] = {
  {
    .name = "Debian",
    // from desktop-base package
    .logofile = "/usr/share/desktop-base/debian-logos/logo-text-256.png",
  }, {
    .name = "Fedora",
    // from redhat-lsb-core package
    .logofile = "/usr/share/pixmaps/fedora-logo.png",
  }, {
    .name = NULL,
    .logofile = NULL,
  },
};

static char*
pipe_getline(const char* cmdline){
  FILE* p = popen(cmdline, "re");
  if(p == NULL){
    fprintf(stderr, "Error running lsb_release -i (%s)\n", strerror(errno));
    return NULL;
  }
  char* buf = malloc(BUFSIZ); // gatesv("BUFSIZ bytes is enough for anyone")
  if(fgets(buf, BUFSIZ, p) == NULL){
    fprintf(stderr, "Error reading from lsb_release -i (%s)\n", strerror(errno));
    fclose(p);
    free(buf);
    return NULL;
  }
  if(fclose(p)){
    fprintf(stderr, "Error closing pipe (%s)\n", strerror(errno));
    free(buf);
    return NULL;
  }
  return buf;
}

static char*
pipe_lsbrelease(const char* cmdline){
  char* buf = pipe_getline(cmdline);
  if(buf == NULL){
    return NULL;
  }
  const char* colon = strchr(buf, ':');
  if(colon == NULL){
    free(buf);
    return NULL;
  }
  const char* distro = ++colon;
  while(*distro && isspace(*distro)){
    ++distro;
  }
  char* nl = strchr(distro, '\n');
  if(nl == NULL){
    free(buf);
    return NULL;
  }
  *nl = '\0';
  char* ret = strdup(distro);
  free(buf);
  return ret;
}

static const distro_info*
getdistro(void){
  const distro_info* dinfo = NULL;
  char* buf = pipe_lsbrelease("lsb_release -i");
  if(buf == NULL){
    return NULL;
  }
  for(dinfo = distros ; dinfo->name ; ++dinfo){
    if(strcmp(dinfo->name, buf) == 0){
      break;
    }
  }
  if(dinfo->name == NULL){
    dinfo = NULL;
  }
  free(buf);
  return dinfo;
}

static int
unix_getusername(fetched_info* fi){
  if( (fi->username = getenv("LOGNAME")) ){
    if( (fi->username = strdup(fi->username)) ){
      return 0;
    }
  }
  uid_t uid = getuid();
  struct passwd* p = getpwuid(uid);
  if(p == NULL){
    return -1;
  }
  fi->username = strdup(p->pw_name);
  return 0;
}

static int
unix_gethostname(fetched_info* fi){
  if(gethostname(fi->hostname, sizeof(fi->hostname)) == 0){
    char* fqdn = strchr(fi->hostname, '.');
    if(fqdn){
      *fqdn = '\0';
    }
    return 0;
  }
  return -1;
}

static const distro_info*
linux_ncneofetch(fetched_info* fi){
  const distro_info* dinfo = getdistro();
  if(dinfo == NULL){
    return NULL;
  }
  fi->distro_release = pipe_lsbrelease("lsb_release -r");
  unix_gethostname(fi);
  unix_getusername(fi);
  return dinfo;
}

typedef enum {
  NCNEO_LINUX,
  NCNEO_FREEBSD,
  NCNEO_UNKNOWN,
} ncneo_kernel_e;

static ncneo_kernel_e
get_kernel(fetched_info* fi){
  struct utsname uts;
  if(uname(&uts)){
    fprintf(stderr, "Failure invoking uname (%s)\n", strerror(errno));
    return -1;
  }
  fi->kernel = strdup(uts.sysname);
  fi->kernver = strdup(uts.release);
  if(strcmp(uts.sysname, "Linux") == 0){
    return NCNEO_LINUX;
  }else if(strcmp(uts.sysname, "FreeBSD") == 0){
    return NCNEO_FREEBSD;
  }
  fprintf(stderr, "Unknown operating system via uname: %s\n", uts.sysname);
  return NCNEO_UNKNOWN;
}

static struct ncplane*
display(struct notcurses* nc, const distro_info* dinfo){
  if(dinfo->logofile){
    int dimy, dimx;
    nc_err_e err;
    struct ncvisual* ncv = ncvisual_from_file(dinfo->logofile, &err);
    if(ncv == NULL){
      fprintf(stderr, "Error opening logo file at %s\n", dinfo->logofile);
      return NULL;
    }
    struct ncvisual_options vopts = {
      .scaling = NCSCALE_SCALE,
      .blitter = NCBLIT_2x2,
      .n = notcurses_stddim_yx(nc, &dimy, &dimx),
    };
    int y, x, scaley, scalex;
    ncvisual_geom(nc, ncv, &vopts, &y, &x, &scaley, &scalex);
    if(y / scaley < dimy){
      vopts.y = (dimy - (y + (scaley - 1)) / scaley) / 2;
    }
    if(x / scalex < dimx){
      vopts.x = (dimx - (x + (scalex - 1)) / scalex) / 2;
    }
    if(ncvisual_render(nc, ncv, &vopts) == NULL){
      ncvisual_destroy(ncv);
      return NULL;
    }
    ncvisual_destroy(ncv);
  }
  return 0;
}

static const distro_info*
freebsd_ncneofetch(fetched_info* fi){
  static const distro_info fbsd = {
    .name = "FreeBSD",
    .logofile = NULL, // FIXME
  };
  unix_gethostname(fi);
  unix_getusername(fi);
  return &fbsd;
}

static int
drawpalette(struct notcurses* nc){
  int psize = notcurses_palette_size(nc);
  if(psize > 256){
    psize = 256;
  }
  int dimy, dimx;
  struct ncplane* stdn = notcurses_stddim_yx(nc, &dimy, &dimx);
  if(dimx < 64){
    return -1;
  }
  cell c = CELL_SIMPLE_INITIALIZER(' ');
  // FIXME find a better place to put it
  const int yoff = 2;
  for(int y = yoff ; y < yoff + psize / 64 ; ++y){
    for(int x = (dimx - 64) / 2 ; x < dimx / 2 + 32 ; ++x){
      const int truex = x - (dimx - 64) / 2;
      if((y - yoff) * 64 + truex >= psize){
        break;
      }
      cell_set_bg_palindex(&c, (y - yoff) * 64 + truex);
      ncplane_putc_yx(stdn, y, x, &c);
    }
  }
  return 0;
}

static int
infoplane(struct notcurses* nc, const fetched_info* fi){
  // FIXME look for an area without background logo in it. pick the one
  // closest to the center horizontally, and lowest vertically. if none
  // can be found, just center it on the bottom as we do now
  const int dimy = ncplane_dim_y(notcurses_stdplane(nc));
  const int planeheight = 8;
  const int planewidth = 60;
  struct ncplane* infop = ncplane_aligned(notcurses_stdplane(nc),
                                          planeheight, planewidth,
                                          dimy - (planeheight + 1),
                                          NCALIGN_CENTER, NULL);
  if(infop == NULL){
    return -1;
  }
  ncplane_set_fg_rgb(infop, 0xd0, 0xd0, 0xd0);
  ncplane_set_attr(infop, NCSTYLE_UNDERLINE);
  ncplane_printf_aligned(infop, 1, NCALIGN_LEFT, " %s %s", fi->kernel, fi->kernver);
  ncplane_printf_aligned(infop, 1, NCALIGN_RIGHT, "%s %s ",
                         fi->distro->name, fi->distro_release);
  ncplane_set_attr(infop, NCSTYLE_NONE);
#ifdef __linux__
  struct sysinfo sinfo;
  sysinfo(&sinfo);
  char totalmet[BPREFIXSTRLEN + 1], usedmet[BPREFIXSTRLEN + 1];
  bprefix(sinfo.totalram, 1, totalmet, 1);
  bprefix(sinfo.totalram - sinfo.freeram, 1, usedmet, 1);
  ncplane_printf_aligned(infop, 2, NCALIGN_LEFT, " RAM: %s/%s\n", usedmet, totalmet);
  ncplane_printf_aligned(infop, 2, NCALIGN_RIGHT, "Processes: %hu ", sinfo.procs);
#endif
  cell ul = CELL_TRIVIAL_INITIALIZER; cell ur = CELL_TRIVIAL_INITIALIZER;
  cell ll = CELL_TRIVIAL_INITIALIZER; cell lr = CELL_TRIVIAL_INITIALIZER;
  cell hl = CELL_TRIVIAL_INITIALIZER; cell vl = CELL_TRIVIAL_INITIALIZER;
  if(cells_rounded_box(infop, 0, 0, &ul, &ur, &ll, &lr, &hl, &vl)){
    return -1;
  }
  cell_set_fg_rgb(&ul, 0x90, 0x90, 0x90);
  cell_set_fg_rgb(&ur, 0x90, 0x90, 0x90);
  cell_set_fg_rgb(&ll, 0, 0, 0);
  cell_set_fg_rgb(&lr, 0, 0, 0);
  unsigned ctrlword = NCBOXGRAD_BOTTOM | NCBOXGRAD_LEFT | NCBOXGRAD_RIGHT;
  if(ncplane_perimeter(infop, &ul, &ur, &ll, &lr, &hl, &vl, ctrlword)){
    return -1;
  }
  ncplane_home(infop);
  uint64_t channels = 0;
  channels_set_fg_rgb(&channels, 0, 0xff, 0);
  ncplane_hline_interp(infop, &hl, planewidth / 2, ul.channels, channels);
  ncplane_hline_interp(infop, &hl, planewidth / 2, channels, ur.channels);
  cell_release(infop, &ul); cell_release(infop, &ur);
  cell_release(infop, &ll); cell_release(infop, &lr);
  cell_release(infop, &hl); cell_release(infop, &vl);
  ncplane_set_fg_rgb(infop, 0xff, 0xff, 0xff);
  ncplane_set_attr(infop, NCSTYLE_BOLD);
  if(ncplane_printf_aligned(infop, 0, NCALIGN_CENTER, "[ %s@%s ]",
                            fi->username, fi->hostname) < 0){
    return -1;
  }
  channels_set_fg_rgb(&channels, 0, 0, 0);
  channels_set_bg_rgb(&channels, 0x50, 0x50, 0x50);
  ncplane_set_base(infop, " ", 0, channels);
  return 0;
}

static int
ncneofetch(struct notcurses* nc){
  fetched_info fi = {};
  ncneo_kernel_e kern = get_kernel(&fi);
  switch(kern){
    case NCNEO_LINUX:
      fi.distro = linux_ncneofetch(&fi);
      break;
    case NCNEO_FREEBSD:
      fi.distro = freebsd_ncneofetch(&fi);
      break;
    case NCNEO_UNKNOWN:
      break;
  }
  if(fi.distro == NULL){
    return -1;
  }
  if(display(nc, fi.distro)){
    return -1; // FIXME soldier on, perhaps?
  }
  if(infoplane(nc, &fi)){
    return -1;
  }
  if(drawpalette(nc)){
    return -1;
  }
  if(notcurses_render(nc)){
    return -1;
  }
  return 0;
}

int main(void){
  if(setlocale(LC_ALL, "") == NULL){
    fprintf(stderr, "Warning: couldn't set locale based off LANG\n");
  }
  struct notcurses_options nopts = {
    .flags = NCOPTION_INHIBIT_SETLOCALE | NCOPTION_NO_ALTERNATE_SCREEN |
              NCOPTION_SUPPRESS_BANNERS,
  };
  struct notcurses* nc = notcurses_init(&nopts, NULL);
  if(nc == NULL){
    return EXIT_FAILURE;
  }
  int r = ncneofetch(nc);
  if(notcurses_stop(nc)){
    return EXIT_FAILURE;
  }
  return r ? EXIT_FAILURE : EXIT_SUCCESS;
}
