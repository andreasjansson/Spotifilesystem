/* 
 * Spotifilesystem
 * Copyright (C) 2011   andreas@jansson.me.uk
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

// This code is just horrible at the moment, but that will probably
// change very soon.
// I don't even have a Makefile.
// gcc  -ospfs -Wall -g -lfuse -lpthread -lspotify -D_FILE_OFFSET_BITS=64 spfs.c

// to compile, you need appkey.c and password.c provided.

#define FUSE_USE_VERSION  26
   
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <libspotify/api.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "appkey.c"
#include "password.c"

#define MAX_FILE_SIZE 50000000
#define MAX_NAME_LENGTH 512
#define MAX_SEARCH_RESULTS 1000

sp_session *session;

int dirty_global_argc;
char **dirty_global_argv;

static int g_playback_done;
/// Synchronization mutex for the main thread
static pthread_mutex_t g_notify_mutex;
/// Synchronization condition variable for the main thread
static pthread_cond_t g_notify_cond;
/// Synchronization variable telling the main thread to process events
static int g_notify_do;

int seconds = 0;

typedef struct file {
  char name[MAX_NAME_LENGTH];
  sp_track *track;
  struct file *next;
} file_t;

typedef struct folder {
  char name[MAX_NAME_LENGTH];
  file_t *files;
  struct folder *next;
} folder_t;

folder_t *folders = NULL;

char frame_buf[MAX_FILE_SIZE]; // 5M
unsigned int request_size = 0;
unsigned int frame_buf_size = 0;

int track_ended = 1;

pthread_mutex_t read_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;

/**
 * This callback is called from an internal libspotify thread to ask us to
 * reiterate the main loop.
 *
 * We notify the main thread using a condition variable and a protected variable.
 *
 * @sa sp_session_callbacks#notify_main_thread
 */
static void notify_main_thread(sp_session *sess)
{
  pthread_mutex_lock(&g_notify_mutex);
  g_notify_do = 1;
  pthread_cond_signal(&g_notify_cond);
  pthread_mutex_unlock(&g_notify_mutex);
}

/**
 * This callback is used from libspotify whenever there is PCM data available.
 *
 * @sa sp_session_callbacks#music_delivery
 */
static int music_delivery(sp_session *sess, const sp_audioformat *format,
                          const void *frames, int num_frames)
{
  size_t s;

  if (num_frames == 0)
    return 0; // Audio discontinuity, do nothing

  s = num_frames * sizeof(int16_t) * format->channels;
  //  fwrite(frames, s, 1, stdout);

  pthread_mutex_lock(&read_mutex);

  if(request_size > 0 && frame_buf_size >= request_size)
    pthread_cond_signal(&read_cond);

  memcpy(frame_buf + frame_buf_size, frames, s);
  frame_buf_size += s;

  pthread_mutex_unlock(&read_mutex);
  
  return num_frames;
}


/**
 * This callback is used from libspotify when the current track has ended
 *
 * @sa sp_session_callbacks#end_of_track
 */
static void end_of_track(sp_session *sess)
{
  printf("Spotify: End of track\n");

  pthread_mutex_lock(&g_notify_mutex);
  g_playback_done = 1;
  track_ended = 1;
  pthread_cond_signal(&g_notify_cond);
  pthread_mutex_unlock(&g_notify_mutex);
}


/**
 * Notification that some other connection has started playing on this account.
 * Playback has been stopped.
 *
 * @sa sp_session_callbacks#play_token_lost
 */
static void play_token_lost(sp_session *sess)
{
  sp_session_player_unload(sess);
}

void write_wav_header(char *buf)
{
  char header[] = { 
    0x52, 0x49, 0x46, 0x46, 0x24, 0x40, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20,
    0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x44, 0xac, 0x00, 0x00, 0x10, 0xb1, 0x02, 0x00,
    0x04, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0xff, 0xff, 0xff, 0xff
  };  

  memcpy(buf, header, 44);
}

void search_complete(sp_search *result, void *userdata)
{
  int i;
  folder_t *folder = (folder_t *)userdata;
  file_t *file;

  printf("Spotify: Search complete, tracks: %d\n", sp_search_num_tracks(result));

  for(i = 0; i < sp_search_num_tracks(result); i ++) {
    file = calloc(1, sizeof(file_t));
    file->track = sp_search_track(result, i);
    strncpy(file->name, sp_artist_name(sp_track_artist(file->track, 0)), MAX_NAME_LENGTH);
    strcat(file->name, " - ");
    strcat(file->name, sp_track_name(file->track));
    strcat(file->name, ".wav");

    printf("Spotify: Adding track to folder: %s\n", file->name);

    if(i == 0)
      folder->files = file;
    else {
      file->next = folder->files;
      folder->files = file;
    }
  }
}

static void logged_in(sp_session *sess, sp_error error)
{
  printf("Spotify: Logged in\n");
}

static sp_session_callbacks session_callbacks = {
  .logged_in = &logged_in,
  .notify_main_thread = &notify_main_thread,
  .music_delivery = &music_delivery,
  .metadata_updated = NULL,
  .play_token_lost = &play_token_lost,
  .log_message = NULL,
  .end_of_track = &end_of_track,
};

static sp_session_config session_config = {
  .api_version = SPOTIFY_API_VERSION,
  .cache_location = "tmp",
  .settings_location = "tmp",
  .application_key = g_appkey,
  .application_key_size = 0, // Set in main()
  .user_agent = "spotifilesystem",
  .callbacks = &session_callbacks,
  NULL,
};


static int spfs_mkdir(const char *path, mode_t mode)
{
  // only allow one "/", at the beginning
  if(strstr(path + 1, "/") != NULL) {
    errno = EIO;
    return -1;
  }

  folder_t *folder = calloc(1, sizeof(folder_t));
  strncpy(folder->name, path + 1, MAX_NAME_LENGTH);

  /*
    char search_string[MAX_NAME_LENGTH];
    memset(search_string, '\0', MAX_NAME_LENGTH * sizeof(char));
    strncpy(search_string, path + 1, strlen(path) - 5);
    printf("Spotify: Searching for %s\n", search_string);
    sp_search_create(session, search_string, 0, 1, 0, 0, 0, 0,
    &search_complete, (void *)folder);
  */

  printf("Spotify: Searching for %s\n", path + 1);

  sp_search_create(session, path + 1, 0, MAX_SEARCH_RESULTS, 0, 0, 0, 0,
                   &search_complete, (void *)folder);
  
  if(!folders) {
    folders = folder;
  }
  else {
    folder->next = folders;
    folders = folder;
  }

  return 0;
}

folder_t *find_folder(const char *name)
{
  folder_t *folder;
  for(folder = folders; folder; folder = folder->next) {
    if(strcmp(name, folder->name) == 0)
      return folder;
  }

  return NULL;
}

// name e.g. "hello/The Cat Empire - Hello.wav"
file_t *find_file(const char *name)
{
  folder_t *folder;
  file_t *file;
  int offset;

  for(folder = folders; folder; folder = folder->next) {
    if(strstr(name, folder->name) == name && strlen(name) > strlen(folder->name) + 1 &&
       *(name + strlen(folder->name)) == '/') {
      offset = strlen(folder->name) + 1;
      for(file = folder->files; file; file = file->next) {
        if(strcmp(name + offset, file->name) == 0)
          return file;
      }
    }
  }

  return NULL;
  
}

static int spfs_getattr(const char *path, struct stat *stbuf)
{
  folder_t *folder;

  memset(stbuf, 0, sizeof(struct stat));
  if(strcmp(path, "/") == 0) {
    stbuf->st_mode = S_IFDIR | 0555;
    stbuf->st_nlink = 2;

    return 0;
  }
  else {

    if((folder = find_folder(path + 1))) {
      stbuf->st_mode = S_IFDIR | 0555;
      stbuf->st_nlink = 2;

      return 0;
    }

    file_t *file;
    if((file = find_file(path + 1))) {
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = MAX_FILE_SIZE;

      return 0;
    }
  }
  
  return -ENOENT;
}
  
static int spfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
  folder_t *folder;

  (void) offset;
  (void) fi;

  if(strcmp(path, "/") == 0) {
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    for(folder = folders; folder; folder = folder->next) {
      filler(buf, folder->name, NULL, 0);
    }
  }
  else {
    folder = find_folder(path + 1);
    if(folder) {

      filler(buf, ".", NULL, 0);
      filler(buf, "..", NULL, 0);

      file_t *file;
      for(file = folder->files; file; file = file->next) {
        filler(buf, file->name, NULL, 0);
      }

    }
    else {
      return -ENOENT;
    }
  }

  return 0;
}
  
static int spfs_open(const char *path, struct fuse_file_info *fi)
{
  file_t *file = find_file(path + 1);
  if(!file)
    return -ENOENT;

  if(!track_ended)
    sp_session_player_unload(session);  

  printf("Spotify: Loading %s by %s\n", sp_track_name(file->track), sp_artist_name(sp_track_artist(file->track, 0)));

  sp_session_player_load(session, file->track);
  sp_session_player_play(session, 1);

  write_wav_header(frame_buf);
  frame_buf_size = 44;

  track_ended = 0;
  
  return 0;
}
  
static int spfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
  (void) fi;

  file_t *file = find_file(path + 1);
  if(!file)
    return -ENOENT;

  request_size = size;

  pthread_mutex_lock(&read_mutex);

  if(!track_ended)
    pthread_cond_wait(&read_cond, &read_mutex);

  printf("Spotify: Read size: %d, frame_buf_size: %d\n", size, frame_buf_size);

  if(track_ended && size > frame_buf_size) {
    size = frame_buf_size;
    frame_buf_size = 0;
  }

  memcpy(buf, frame_buf, size);

  if(!(track_ended && size > frame_buf_size)) {
    frame_buf_size = frame_buf_size - size;
    memcpy(frame_buf, frame_buf + size, frame_buf_size);
  }

  pthread_mutex_unlock(&read_mutex);

  return size;
}
  
static struct fuse_operations spfs_oper = {
  .getattr   = spfs_getattr,
  .readdir = spfs_readdir,
  .open   = spfs_open,
  .read   = spfs_read,
  .mkdir = spfs_mkdir
};
  

void *start_spotify(void *arg)
{
  printf("Spotify: Started\n");
  int next_timeout = 0;
  sp_error err;

  /* Create session */
  session_config.application_key_size = g_appkey_size;

  err = sp_session_create(&session_config, &session);

  if(err != SP_ERROR_OK) {
    fprintf(stderr, "Unable to create session: %s\n",
            sp_error_message(err));
    return NULL;
  }


  sp_session_login(session, username, password, 1);


  for (;;) {
    if (next_timeout == 0) {
      while(!g_notify_do && !g_playback_done)
        pthread_cond_wait(&g_notify_cond, &g_notify_mutex);
    } else {
      struct timespec ts;

#if _POSIX_TIMERS > 0
      clock_gettime(CLOCK_REALTIME, &ts);
#else
      struct timeval tv;
      gettimeofday(&tv, NULL);
      TIMEVAL_TO_TIMESPEC(&tv, &ts);
#endif
      ts.tv_sec += next_timeout / 1000;
      ts.tv_nsec += (next_timeout % 1000) * 1000000;

      pthread_cond_timedwait(&g_notify_cond, &g_notify_mutex, &ts);
    }

    g_notify_do = 0;
    pthread_mutex_unlock(&g_notify_mutex);

    if (g_playback_done) {
      //      track_ended();
      g_playback_done = 0;
    }

    do {
      sp_session_process_events(session, &next_timeout);
    } while (next_timeout == 0);

    pthread_mutex_lock(&g_notify_mutex);
  }
}

void *start_fuse(void *arg)
{
  printf("FUSE: Started\n");
  fuse_main(dirty_global_argc, dirty_global_argv,
            &spfs_oper, NULL);

  return NULL;
}


int main(int argc, char *argv[])
{
  pthread_t spotify_thread, fuse_thread;

  dirty_global_argc = argc;
  dirty_global_argv = argv;
  pthread_create(&spotify_thread, NULL, start_spotify, NULL);
  pthread_create(&fuse_thread, NULL, start_fuse, NULL);

  pthread_join(spotify_thread, NULL);
  pthread_join(fuse_thread, NULL);

  return 0;
}
