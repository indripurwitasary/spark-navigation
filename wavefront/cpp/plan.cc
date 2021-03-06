/*
 *  Player - One Hell of a Robot Server
 *  Copyright (C) 2003
 *     Andrew Howard
 *     Brian Gerkey    
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */


/**************************************************************************
 * Desc: Path planning
 * Author: Andrew Howard
 * Date: 10 Oct 2002
 * CVS: $Id: plan.c 9139 2014-02-18 02:50:05Z jpgr87 $
**************************************************************************/

//#include <config.h>

// This header MUST come before <openssl/md5.h>
#include <sys/types.h>

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  #include <openssl/md5.h>
#endif

#include <cassert>
#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include <algorithm> // for max/min

#include <libplayercommon/playercommon.h>

#include "plan.h"

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
// length of the hash, in unsigned ints
#define HASH_LEN (MD5_DIGEST_LENGTH / sizeof(unsigned int))
#endif

#if defined (WIN32)
  #include <replace/replace.h>
  #include <winsock2.h> // For struct timeval
#else
  #include <sys/time.h>
#endif

#if 0
void draw_cspace(plan_t* plan, const char* fname);
#endif

// Create a planner
plan_t::plan_t(double _abs_min_radius,
               double _max_radius, double _dist_penalty,
               double _hysteresis_factor) :
		abs_min_radius(_abs_min_radius),
		max_radius(_max_radius),
		dist_penalty(_dist_penalty),
		hysteresis_factor(_hysteresis_factor),
	 	min_x(0), min_y(0), max_x(0), max_y(0),
	 	scale(0.0),
	 	cells(NULL),
	 	dist_kernel(NULL), dist_kernel_width(0)
{
  size.x = 0; size.y = 0;
  origin.x = 0; origin.y = 0;
  path.reserve(1000);
  lpath.reserve(100);
  waypoints.reserve(100);
}

// Destroy a planner
plan_t::~plan_t()
{
  if (cells)
    delete [] cells;
  if(dist_kernel)
    delete [] dist_kernel;
}

// Copy the planner
plan_t::plan_t(const plan_t & plan) :
	abs_min_radius(plan.abs_min_radius),
	max_radius(plan.max_radius),
	dist_penalty(plan.dist_penalty),
	hysteresis_factor(plan.hysteresis_factor),
	min_x(0), min_y(0), max_x(0), max_y(0),
	size(plan.size),
	origin(plan.origin),
	scale(plan.scale),
	cells(NULL),
	dist_kernel(NULL), dist_kernel_width(0)
{
  path.reserve(1000);
  lpath.reserve(100);
  waypoints.reserve(100);

  // Now get the map data
  // Allocate space for map cells
  this->cells = new plan_cell_t[this->size.x * this->size.y];

  // Do initialisation
  this->init();

  // Copy the map data
  for (int i = 0; i < this->size.x * this->size.y; ++i)
  {
    this->cells[i].occ_dist = plan.cells[i].occ_dist;
	this->cells[i].occ_state = plan.cells[i].occ_state;
	this->cells[i].occ_state_dyn = plan.cells[i].occ_state_dyn;
	this->cells[i].occ_dist_dyn = plan.cells[i].occ_dist_dyn;
  }
}

void
plan_t::set_obstacles(double* obs, size_t num)
{
  double t0 = get_time();

  // Start with static obstacle data
  plan_cell_t* cell = cells;
  for(int j=0;j<size.y*size.x;j++,cell++)
  {
    cell->occ_state_dyn = cell->occ_state;
    cell->occ_dist_dyn = cell->occ_dist;
    cell->mark = false;
  }

  // Expand around the dynamic obstacle pts
  for(size_t i=0;i<num;i++)
  {
    // Convert to grid coords
    int gx = GXWX(obs[2*i]);
    int gy = GYWY(obs[2*i+1]);

    if(!VALID(gx,gy))
      continue;

    cell = cells + INDEX(gx,gy);

    if(cell->mark)
      continue;

    cell->mark = true;
    cell->occ_state_dyn = 1;
    cell->occ_dist_dyn = 0.0;

    float * p = dist_kernel;
    for (int dj = -dist_kernel_width/2;
             dj <= dist_kernel_width/2;
             dj++)
    {
      plan_cell_t * ncell = cell + -dist_kernel_width/2 + dj*size.x;
      for (int di = -dist_kernel_width/2;
               di <= dist_kernel_width/2;
               di++, p++, ncell++)
      {
        if(!VALID_BOUNDS(cell->ci+di,cell->cj+dj))
          continue;

        if(*p < ncell->occ_dist_dyn)
          ncell->occ_dist_dyn = *p;
      }
    }
  }

  double t1 = get_time();
  //printf("plan_set_obstacles: %.6lf\n", t1-t0);
}

void
plan_t::compute_dist_kernel()
{
  // Compute variable sized kernel, for use in propagating distance from
  // obstacles
  dist_kernel_width = 1 + 2 * (int)ceil(max_radius / scale);

  if(dist_kernel) delete [] dist_kernel;
  dist_kernel = new float[dist_kernel_width*dist_kernel_width];

  float * p = dist_kernel;
  for(int j=-dist_kernel_width/2;j<=dist_kernel_width/2;j++)
  {
    for(int i=-dist_kernel_width/2;i<=dist_kernel_width/2;i++,p++)
    {
      *p = (float) (hypot(i,j) * scale);
    }
  }
  // also compute a 3x3 kernel, used when propagating distance from goal
  p = dist_kernel_3x3;
  for(int j=-1;j<=1;j++)
  {
    for(int i=-1;i<=1;i++,p++)
    {
      *p = (float) (hypot(i,j) * scale);
    }
  }
}

// Initialize the plan
void plan_t::init()
{
  printf("scale: %.3lf\n", scale);

  plan_cell_t *cell = cells;
  for (int j = 0; j < size.y; j++)
  {
    for (int i = 0; i < size.x; i++, cell++)
    {
      cell->ci = i;
      cell->cj = j;
      cell->occ_state_dyn = cell->occ_state;
      cell->occ_dist_dyn = cell->occ_dist;
      cell->plan_cost = PLAN_MAX_COST;
      cell->plan_next = NULL;
      cell->lpathmark = false;
    }
  }
  waypoints.clear();

  compute_dist_kernel();

  set_bounds(0, 0, size.x - 1, size.y - 1);
}


// Reset the plan
void plan_t::reset()
{
  for (int j = min_y; j <= max_y; j++)
  {
    for (int i = min_x; i <= max_x; i++)
    {
      plan_cell_t *cell = cells + INDEX(i,j);
      cell->plan_cost = PLAN_MAX_COST;
      cell->plan_next = NULL;
      cell->mark = false;
    }
  }
  waypoints.clear();
}

void
plan_t::set_bounds(int min_x, int min_y, int max_x, int max_y)
{
  assert(min_x <= max_x);
  assert(min_y <= max_y);

  this->min_x = std::min(size.x-1, std::max(0,min_x));
  this->min_y = std::min(size.y-1, std::max(0,min_y));
  this->max_x = std::min(size.x-1, std::max(0,max_x));
  this->max_y = std::min(size.y-1, std::max(0,max_y));

  //printf("new bounds: (%d,%d) -> (%d,%d)\n",
         //plan->min_x, plan->min_y,
         //plan->max_x, plan->max_y);
}

bool
plan_t::check_inbounds(double x, double y) const
{
  int gx = GXWX(x);
  int gy = GYWY(y);

  return ((gx >= min_x) && (gx <= max_x) &&
          (gy >= min_y) && (gy <= max_y));
}

void
plan_t::set_bbox(double padding, double min_size,
                 double x0, double y0, double x1, double y1)
{
  int gx0, gy0, gx1, gy1;
  int min_x, min_y, max_x, max_y;
  int sx, sy;
  int dx, dy;
  int gmin_size;
  int gpadding;

  gx0 = GXWX(x0);
  gy0 = GYWY(y0);
  gx1 = GXWX(x1);
  gy1 = GYWY(y1);

  // Make a bounding box to include both points.
  min_x = std::min(gx0, gx1);
  min_y = std::min(gy0, gy1);
  max_x = std::max(gx0, gx1);
  max_y = std::max(gy0, gy1);

  // Make sure the min_size is achievable
  gmin_size = (int)ceil(min_size / scale);
  gmin_size = std::min(gmin_size, std::min(size.x-1, size.y-1));

  // Add padding
  gpadding = (int)ceil(padding / scale);
  min_x -= gpadding / 2;
  min_x = std::max(min_x, 0);
  max_x += gpadding / 2;
  max_x = std::min(max_x, size.x - 1);
  min_y -= gpadding / 2;
  min_y = std::max(min_y, 0);
  max_y += gpadding / 2;
  max_y = std::min(max_y, size.y - 1);

  // Grow the box if necessary to achieve the min_size
  sx = max_x - min_x;
  while(sx < gmin_size)
  {
    dx = gmin_size - sx;
    min_x -= (int)ceil(dx / 2.0);
    max_x += (int)ceil(dx / 2.0);

    min_x = std::max(min_x, 0);
    max_x = std::min(max_x, size.x-1);

    sx = max_x - min_x;
  }
  sy = max_y - min_y;
  while(sy < gmin_size)
  {
    dy = gmin_size - sy;
    min_y -= (int)ceil(dy / 2.0);
    max_y += (int)ceil(dy / 2.0);

    min_y = std::max(min_y, 0);
    max_y = std::min(max_y, size.y-1);

    sy = max_y - min_y;
  }

  set_bounds(min_x, min_y, max_x, max_y);
}

void
plan_t::compute_cspace()
{
  puts("Generating C-space...");

  // FIXME: this should iterate across size_x/y and not min_x/y..max_x/y (?).

  for (int j = min_y; j <= max_y; j++)
  {
    plan_cell_t *cell = cells + INDEX(0, j);
    for (int i = min_x; i <= max_x; i++, cell++)
    {
      if (cell->occ_state < 0)
        continue;

      float *p = dist_kernel;
      for (int dj = -dist_kernel_width/2;
               dj <= dist_kernel_width/2;
               dj++)
      {
        plan_cell_t *ncell = cell + -dist_kernel_width/2 + dj*size.x;
        for (int di = -dist_kernel_width/2;
                 di <= dist_kernel_width/2;
                 di++, p++, ncell++)
        {
          if(!VALID_BOUNDS(i+di,j+dj))
            continue;

          if(*p < ncell->occ_dist)
            ncell->occ_dist_dyn = ncell->occ_dist = *p;
        }
      }
    }
  }
}

#if 0
#include <gdk-pixbuf/gdk-pixbuf.h>

void
draw_cspace(plan_t* plan, const char* fname)
{
  GdkPixbuf* pixbuf;
  GError* error = NULL;
  guchar* pixels;
  int p;
  int paddr;
  int i, j;

  pixels = (guchar*)malloc(sizeof(guchar)*plan->size.x*plan->size.y*3);

  p=0;
  for(j=plan->size.y-1;j>=0;j--)
  {
    for(i=0;i<plan->size.x;i++,p++)
    {
      paddr = p * 3;
      //if(plan->cells[PLAN_INDEX(i,j)].occ_state == 1)
      if(plan->cells[INDEX(i,j)].occ_dist < plan->abs_min_radius)
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 0;
      }
      else if(plan->cells[INDEX(i,j)].occ_dist < plan->max_radius)
      {
        pixels[paddr] = 0;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
      else
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 255;
        pixels[paddr+2] = 255;
      }
    }
  }

  pixbuf = gdk_pixbuf_new_from_data(pixels, 
                                    GDK_COLORSPACE_RGB,
                                    0,8,
                                    plan->size.x,
                                    plan->size.y,
                                    plan->size.x * 3,
                                    NULL, NULL);

  gdk_pixbuf_save(pixbuf,fname,"png",&error,NULL);
  gdk_pixbuf_unref(pixbuf);
  free(pixels);
}

        void
draw_path(plan_t* plan, double lx, double ly, const char* fname)
{
  GdkPixbuf* pixbuf;
  GError* error = NULL;
  guchar* pixels;
  int p;
  int paddr;
  int i, j;
  plan_cell_t* cell;

  pixels = (guchar*)malloc(sizeof(guchar)*plan->size.x*plan->size.y*3);

  p=0;
  for(j=plan->size.y-1;j>=0;j--)
  {
    for(i=0;i<plan->size.x;i++,p++)
    {
      paddr = p * 3;
      if(plan->cells[INDEX(i,j)].occ_state == 1)
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 0;
      }
      else if(plan->cells[INDEX(i,j)].occ_dist < plan->max_radius)
      {
        pixels[paddr] = 0;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
      else
      {
        pixels[paddr] = 255;
        pixels[paddr+1] = 255;
        pixels[paddr+2] = 255;
      }
      /*
         if((7*plan->cells[PLAN_INDEX(i,j)].plan_cost) > 255)
         {
         pixels[paddr] = 0;
         pixels[paddr+1] = 0;
         pixels[paddr+2] = 255;
         }
         else
         {
         pixels[paddr] = 255 - 7*plan->cells[PLAN_INDEX(i,j)].plan_cost;
         pixels[paddr+1] = 0;
         pixels[paddr+2] = 0;
         }
       */
    }
  }

  for(i=0;i<plan->path_count;i++)
  {
    cell = plan->path[i];
    
    paddr = 3*INDEX(cell->ci,plan->size.y - cell->cj - 1);
    pixels[paddr] = 0;
    pixels[paddr+1] = 255;
    pixels[paddr+2] = 0;
  }

  for(i=0;i<plan->lpath_count;i++)
  {
    cell = plan->lpath[i];
    
    paddr = 3*INDEX(cell->ci,plan->size.y - cell->cj - 1);
    pixels[paddr] = 255;
    pixels[paddr+1] = 0;
    pixels[paddr+2] = 255;
  }

  /*
  for(p=0;p<plan->waypoint_count;p++)
  {
    cell = plan->waypoints[p];
    for(j=-3;j<=3;j++)
    {
      cj = cell->cj + j;
      for(i=-3;i<=3;i++)
      {
        ci = cell->ci + i;
        paddr = 3*PLAN_INDEX(ci,plan->size.y - cj - 1);
        pixels[paddr] = 255;
        pixels[paddr+1] = 0;
        pixels[paddr+2] = 255;
      }
    }
  }
  */

  pixbuf = gdk_pixbuf_new_from_data(pixels, 
                                    GDK_COLORSPACE_RGB,
                                    0,8,
                                    plan->size.x,
                                    plan->size.y,
                                    plan->size.x * 3,
                                    NULL, NULL);
  
  gdk_pixbuf_save(pixbuf,fname,"png",&error,NULL);
  gdk_pixbuf_unref(pixbuf);
  free(pixels);
}
#endif

// Construct the configuration space from the occupancy grid.
// This treats both occupied and unknown cells as bad.
// 
// If cachefile is non-NULL, then we try to read the c-space from that
// file.  If that fails, then we construct the c-space as per normal and
// then write it out to cachefile.
#if 0
void 
plan_update_cspace(plan_t *plan, const char* cachefile)
{
#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  unsigned int hash[HASH_LEN];
  plan_md5(hash, plan);
  if(cachefile && strlen(cachefile))
  {
    PLAYER_MSG1(2,"Trying to read c-space from file %s", cachefile);
    if(plan_read_cspace(plan,cachefile,hash) == 0)
    {
      // Reading from the cache file worked; we're done here.
      PLAYER_MSG1(2,"Successfully read c-space from file %s", cachefile);
#if 0
      draw_cspace(plan,"plan_cspace.png");
#endif
      return;
    }
    PLAYER_MSG1(2, "Failed to read c-space from file %s", cachefile);
  }
#endif

  //plan_update_cspace_dp(plan);
  plan_update_cspace_naive(plan);

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
  if(cachefile)
    plan_write_cspace(plan,cachefile, (unsigned int*)hash);
#endif

  PLAYER_MSG0(2,"Done.");

#if 0
  draw_cspace(plan,"plan_cspace.png");
#endif
}

#if HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO
// Write the cspace occupancy distance values to a file, one per line.
// Read them back in with plan_read_cspace().
// Returns non-zero on error.
int 
plan_write_cspace(plan_t *plan, const char* fname, unsigned int* hash)
{
  plan_cell_t* cell;
  int i,j;
  FILE* fp;

  if(!(fp = fopen(fname,"w+")))
  {
    PLAYER_MSG2(2,"Failed to open file %s to write c-space: %s",
                fname,strerror(errno));
    return(-1);
  }

  fprintf(fp,"%d\n%d\n", plan->size.x, plan->size.y);
  fprintf(fp,"%.3lf\n%.3lf\n", plan->origin.x, plan->origin.y);
  fprintf(fp,"%.3lf\n%.3lf\n", plan->scale,plan->max_radius);
  for(i=0;i<HASH_LEN;i++)
    fprintf(fp,"%08X", hash[i]);
  fprintf(fp,"\n");

  for(j = 0; j < plan->size.y; j++)
  {
    for(i = 0; i < plan->size.x; i++)
    {
      cell = plan->cells + INDEX(i, j);
      fprintf(fp,"%.3f\n", cell->occ_dist);
    }
  }

  fclose(fp);
  return(0);
}

// Read the cspace occupancy distance values from a file, one per line.
// Write them in first with plan_read_cspace().
// Returns non-zero on error.
int 
plan_read_cspace(plan_t *plan, const char* fname, unsigned int* hash)
{
  plan_cell_t* cell;
  int i,j;
  FILE* fp;
  int size.x, size.y;
  double origin.x, origin.y;
  double scale, max_radius;
  unsigned int cached_hash[HASH_LEN];

  if(!(fp = fopen(fname,"r")))
  {
    PLAYER_MSG1(2,"Failed to open file %s", fname);
    return(-1);
  }
  
  /* Read out the metadata */
  if((fscanf(fp,"%d", &size.x) < 1) ||
     (fscanf(fp,"%d", &size.y) < 1) ||
     (fscanf(fp,"%lf", &origin.x) < 1) ||
     (fscanf(fp,"%lf", &origin.y) < 1) ||
     (fscanf(fp,"%lf", &scale) < 1) ||
     (fscanf(fp,"%lf", &max_radius) < 1))
  {
    PLAYER_MSG1(2,"Failed to read c-space metadata from file %s", fname);
    fclose(fp);
    return(-1);
  }

  for(i=0;i<HASH_LEN;i++)
  {
    if(fscanf(fp,"%08X", cached_hash+i) < 1)
    {
      PLAYER_MSG1(2,"Failed to read c-space metadata from file %s", fname);
      fclose(fp);
      return(-1);
    }
  }

  /* Verify that metadata matches */
  if((size.x != plan->size.x) ||
     (size.y != plan->size.y) ||
     (fabs(origin.x - plan->origin.x) > 1e-3) ||
     (fabs(origin.y - plan->origin.y) > 1e-3) ||
     (fabs(scale - plan->scale) > 1e-3) ||
     (fabs(max_radius - plan->max_radius) > 1e-3) ||
     memcmp(cached_hash, hash, sizeof(unsigned int) * HASH_LEN))
  {
    PLAYER_MSG1(2,"Mismatch in c-space metadata read from file %s", fname);
    fclose(fp);
    return(-1);
  }

  for(j = 0; j < plan->size.y; j++)
  {
    for(i = 0; i < plan->size.x; i++)
    {
      cell = plan->cells + INDEX(i, j);
      if(fscanf(fp,"%f", &(cell->occ_dist)) < 1)
      {
        PLAYER_MSG3(2,"Failed to read c-space data for cell (%d,%d) from file %s",
                     i,j,fname);
        fclose(fp);
        return(-1);
      }
    }
  }

  fclose(fp);
  return(0);
}

// Compute the 16-byte MD5 hash of the map data in the given plan
// object.
void
plan_md5(unsigned int* digest, plan_t* plan)
{
  MD5_CTX c;

  MD5_Init(&c);

  MD5_Update(&c,(const unsigned char*)plan->cells,
             (plan->size.x*plan->size.y)*sizeof(plan_cell_t));

  MD5_Final((unsigned char*)digest,&c);
}
#endif // HAVE_OPENSSL_MD5_H && HAVE_LIBCRYPTO

#endif // if 0

double 
plan_t::get_time(void)
{
  struct timeval curr;
  gettimeofday(&curr,NULL);
  return(curr.tv_sec + curr.tv_usec / 1e6);
}

//#define PLAN_WXGX(plan, i) (((i) - plan->size_x / 2) * plan->scale)
//#define PLAN_WYGY(plan, j) (((j) - plan->size_y / 2) * plan->scale)
double plan_t::WXGX(int i) const {
	return (this->origin.x + (i) * this->scale);
}

double plan_t::WYGY(int j) const {
	return (this->origin.y + (j) * this->scale);
}

//#define PLAN_GXWX(plan, x) (floor((x) / plan->scale + 0.5) + plan->size_x / 2)
//#define PLAN_GYWY(plan, y) (floor((y) / plan->scale + 0.5) + plan->size_y / 2)

int plan_t::GXWX(double x) const {
	return (int)((x - this->origin.x) / this->scale + 0.5);
}

int plan_t::GYWY(double y) const {
	return (int)((y - this->origin.y) / this->scale + 0.5);
}

bool plan_t::VALID(int i, int j) const {
	return (i >= 0) && (i < this->size.x) && (j >= 0) && (j < this->size.y);
}

bool plan_t::VALID_BOUNDS(int i, int j) const {
	return (i >= this->min_x) && (i <= this->max_x) && (j >= this->min_y) && (j <= this->max_y);
}

int plan_t::INDEX(int i, int j) const
{
	return i + j * this->size.x;
}
