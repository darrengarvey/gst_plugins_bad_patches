/* GStreamer
 * Copyright (C) 2010 Marc-Andre Lureau <marcandre.lureau@gmail.com>
 *
 * m3u8.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <glib.h>
#include <string.h>

#include <gst/glib-compat-private.h>
#include "gstfragmented.h"
#include "m3u8.h"

#define GST_CAT_DEFAULT fragmented_debug

static GstM3U8 *gst_m3u8_new (void);
static void gst_m3u8_free (GstM3U8 * m3u8);
static gboolean gst_m3u8_update (GstM3U8Client * client, GstM3U8 * m3u8,
    gchar * data, gboolean * updated);
static GstM3U8MediaFile *gst_m3u8_media_file_new (gchar * uri,
    gchar * title, GstClockTime duration, guint sequence);
static void gst_m3u8_media_file_free (GstM3U8MediaFile * self);
gchar *uri_join (const gchar * uri, const gchar * path);

static GstM3U8 *
gst_m3u8_new (void)
{
  GstM3U8 *m3u8;

  m3u8 = g_new0 (GstM3U8, 1);

  return m3u8;
}

static void
gst_m3u8_set_uri (GstM3U8 * self, gchar * uri)
{
  g_return_if_fail (self != NULL);

  if (self->uri)
    g_free (self->uri);
  self->uri = uri;
}

static void
gst_m3u8_free (GstM3U8 * self)
{
  g_return_if_fail (self != NULL);

  g_free (self->uri);
  g_free (self->allowcache);
  g_free (self->codecs);
  g_free (self->key);

  g_list_foreach (self->files, (GFunc) gst_m3u8_media_file_free, NULL);
  g_list_free (self->files);

  g_free (self->last_data);
  g_list_foreach (self->lists, (GFunc) gst_m3u8_free, NULL);
  g_list_free (self->lists);

  g_free (self);
}

static GstM3U8MediaFile *
gst_m3u8_media_file_new (gchar * uri, gchar * title, GstClockTime duration,
    guint sequence)
{
  GstM3U8MediaFile *file;

  file = g_new0 (GstM3U8MediaFile, 1);
  file->uri = uri;
  file->title = title;
  file->duration = duration;
  file->sequence = sequence;

  return file;
}

static void
gst_m3u8_media_file_free (GstM3U8MediaFile * self)
{
  g_return_if_fail (self != NULL);

  g_free (self->title);
  g_free (self->uri);
  g_free (self->key);
  g_free (self);
}

static gboolean
int_from_string (gchar * ptr, gchar ** endptr, gint * val)
{
  gchar *end;
  glong ret;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (val != NULL, FALSE);

  errno = 0;
  ret = strtol (ptr, &end, 10);
  if ((errno == ERANGE && (ret == LONG_MAX || ret == LONG_MIN))
      || (errno != 0 && ret == 0)) {
    GST_WARNING ("%s", g_strerror (errno));
    return FALSE;
  }

  if (ret > G_MAXINT) {
    GST_WARNING ("%s", g_strerror (ERANGE));
    return FALSE;
  }

  if (endptr)
    *endptr = end;

  *val = (gint) ret;

  return end != ptr;
}

static gboolean
double_from_string (gchar * ptr, gchar ** endptr, gdouble * val)
{
  gchar *end;
  gdouble ret;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (val != NULL, FALSE);

  errno = 0;
  ret = strtod (ptr, &end);
  if ((errno == ERANGE && (ret == HUGE_VAL || ret == -HUGE_VAL))
      || (errno != 0 && ret == 0)) {
    GST_WARNING ("%s", g_strerror (errno));
    return FALSE;
  }

  if (!isfinite (ret)) {
    GST_WARNING ("%s", g_strerror (ERANGE));
    return FALSE;
  }

  if (endptr)
    *endptr = end;

  *val = (gint) ret;

  return end != ptr;
}

static gboolean
parse_attributes (gchar ** ptr, gchar ** a, gchar ** v)
{
  gchar *end=NULL, *p;

  g_return_val_if_fail (ptr != NULL, FALSE);
  g_return_val_if_fail (*ptr != NULL, FALSE);
  g_return_val_if_fail (a != NULL, FALSE);
  g_return_val_if_fail (v != NULL, FALSE);

  /* [attribute=value,]* */

  *a = *ptr;
  end = p = g_utf8_strchr (*ptr, -1, ',');
  if(end){
    gchar *q = g_utf8_strchr (*ptr, -1, '"');
    if(q && q<end){
      /* special case, such as CODECS="avc1.77.30, mp4a.40.2" */
      q = g_utf8_next_char (q);
      if(q){
        q = g_utf8_strchr (q, -1, '"');
      }
      if(q){
        end = p = g_utf8_strchr (q, -1, ',');
      }
    }
  }
  if (end) {
    do {
      end = g_utf8_next_char (end);
    } while (end && *end == ' ');
    *p = '\0';
  }

  *v = p = g_utf8_strchr (*ptr, -1, '=');
  if (*v) {
    *v = g_utf8_next_char (*v);
    *p = '\0';
  } else {
    GST_WARNING ("missing = after attribute");
    return FALSE;
  }

  *ptr = end;
  return TRUE;
}

static gint
_m3u8_compare_uri (GstM3U8 * a, gchar * uri)
{
  g_return_val_if_fail (a != NULL, 0);
  g_return_val_if_fail (uri != NULL, 0);

  return g_strcmp0 (a->uri, uri);
}

static gint
gst_m3u8_compare_playlist_by_bitrate (gconstpointer a, gconstpointer b)
{
  return ((GstM3U8 *) (a))->bandwidth - ((GstM3U8 *) (b))->bandwidth;
}

/*
 * @data: a m3u8 playlist text data, taking ownership
 */
static gboolean
gst_m3u8_update (GstM3U8Client * client, GstM3U8 * self, gchar * data, gboolean * updated)
{
  gint val;
  GstClockTime duration;
  gchar *title, *end;
//  gboolean discontinuity;
  GstM3U8 *list;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (data != NULL, FALSE);
  g_return_val_if_fail (updated != NULL, FALSE);

  *updated = TRUE;

  /* check if the data changed since last update */
  if (self->last_data && g_str_equal (self->last_data, data)) {
    GST_DEBUG ("Playlist is the same as previous one");
    *updated = FALSE;
    g_free (data);
    return TRUE;
  }

  if (!g_str_has_prefix (data, "#EXTM3U")) {
    GST_WARNING ("Data doesn't start with #EXTM3U");
    *updated = FALSE;
    g_free (data);
    return FALSE;
  }

  g_free (self->last_data);
  self->last_data = data;

  if (self->files) {
    g_list_foreach (self->files, (GFunc) gst_m3u8_media_file_free, NULL);
    g_list_free (self->files);
    self->files = NULL;
  }

  list = NULL;
  duration = 0;
  title = NULL;
  data += 7;
  while (TRUE) {
    end = g_utf8_strchr (data, -1, '\n');
    if (end)
      *end = '\0';

    if (data[0] != '#') {
      gchar *r;

      if (duration <= 0 && list == NULL) {
        GST_LOG ("%s: got line without EXTINF or EXTSTREAMINF, dropping", data);
        goto next_line;
      }

      data = uri_join (self->uri, data);
      if (data == NULL)
        goto next_line;

      r = g_utf8_strchr (data, -1, '\r');
      if (r)
        *r = '\0';

      if (list != NULL) {
        if (g_list_find_custom (self->lists, data,
                (GCompareFunc) _m3u8_compare_uri)) {
          GST_DEBUG ("Already have a list with this URI");
          gst_m3u8_free (list);
          g_free (data);
        } else {
          gst_m3u8_set_uri (list, data);
          self->lists = g_list_append (self->lists, list);
        }
        list = NULL;
      } else {
        GstM3U8MediaFile *file;
        file =
            gst_m3u8_media_file_new (data, title, duration,
            self->mediasequence++);

        /* set encryption params */
        file->key = g_strdup (self->key);
        if (file->key) {
          guint8 *iv = file->iv + 12;
          GST_WRITE_UINT32_BE (iv, file->sequence);
        }

        duration = 0;
        title = NULL;
        self->files = g_list_append (self->files, file);
      }

    } else if (g_str_has_prefix (data, "#EXT-X-ENDLIST")) {
      self->endlist = TRUE;
    } else if (g_str_has_prefix (data, "#EXT-X-VERSION:")) {
      if (int_from_string (data + 15, &data, &val))
        self->version = val;
    } else if (g_str_has_prefix (data, "#EXT-X-STREAM-INF:")) {
      gchar *v, *a;

      if (list != NULL) {
        GST_WARNING ("Found a list without a uri..., dropping");
        gst_m3u8_free (list);
      }

      list = gst_m3u8_new ();
      data = data + 18;
      while (data && parse_attributes (&data, &a, &v)) {
        if (g_str_equal (a, "BANDWIDTH")) {
          if (!int_from_string (v, NULL, &list->bandwidth))
            GST_WARNING ("Error while reading BANDWIDTH");
        } else if (g_str_equal (a, "PROGRAM-ID")) {
          if (!int_from_string (v, NULL, &list->program_id))
            GST_WARNING ("Error while reading PROGRAM-ID");
        } else if (g_str_equal (a, "CODECS")) {
          g_free (list->codecs);
          list->codecs = g_strdup (v);
        } else if (g_str_equal (a, "RESOLUTION")) {
          if (!int_from_string (v, &v, &list->width))
            GST_WARNING ("Error while reading RESOLUTION width");
          if (!v || *v != 'x') {
            GST_WARNING ("Missing height");
          } else {
            v = g_utf8_next_char (v);
            if (!int_from_string (v, NULL, &list->height))
              GST_WARNING ("Error while reading RESOLUTION height");
          }
        }
      }
    } else if (g_str_has_prefix (data, "#EXT-X-TARGETDURATION:")) {
      if (int_from_string (data + 22, &data, &val))
        self->targetduration = val * GST_SECOND;
    } else if (g_str_has_prefix (data, "#EXT-X-MEDIA-SEQUENCE:")) {
      if (int_from_string (data + 22, &data, &val)){
        self->mediasequence = val;
      }
    } else if (g_str_has_prefix (data, "#EXT-X-DISCONTINUITY")) {
      /* discontinuity = TRUE; */
    } else if (g_str_has_prefix (data, "#EXT-X-PROGRAM-DATE-TIME:")) {
      /* <YYYY-MM-DDThh:mm:ssZ> */
      GST_DEBUG ("FIXME parse date");
    } else if (g_str_has_prefix (data, "#EXT-X-ALLOW-CACHE:")) {
      g_free (self->allowcache);
      self->allowcache = g_strdup (data + 19);
    } else if (g_str_has_prefix (data, "#EXT-X-KEY:")) {
      gchar *v, *a;

      data = data + 11;
      while (data && parse_attributes (&data, &a, &v)) {
        if (g_str_equal (a, "URI")) {
          gchar *key = g_strdup (v);
          gchar *keyp = key;
          int len = strlen (key);

          /* handle the \"key\" case */
          if (key[len - 1] == '"')
            key[len - 1] = '\0';
          if (key[0] == '"')
            key += 1;

          self->key = uri_join (self->uri, key);
          g_free (keyp);
        }
      }
    } else if (g_str_has_prefix (data, "#EXTINF:")) {
      gdouble fval;
      if (!double_from_string (data + 8, &data, &fval)) {
        GST_WARNING ("Can't read EXTINF duration");
        goto next_line;
      }
      duration = fval * (gdouble) GST_SECOND;
      if (duration > self->targetduration)
        GST_WARNING ("EXTINF duration > TARGETDURATION");
      if (!data || *data != ',')
        goto next_line;
      data = g_utf8_next_char (data);
      if (data != end) {
        g_free (title);
        title = g_strdup (data);
      }
    } else {
      GST_LOG ("Ignored line: %s", data);
    }

  next_line:
    if (!end)
      break;
    data = g_utf8_next_char (end);      /* skip \n */
  }

  /* redorder playlists by bitrate */
  if (self->lists) {
    gchar *top_variant_uri = NULL;

    if (!self->current_variant)
      top_variant_uri = GST_M3U8 (self->lists->data)->uri;
    else
      top_variant_uri = GST_M3U8 (self->current_variant->data)->uri;

    self->lists =
        g_list_sort (self->lists,
        (GCompareFunc) gst_m3u8_compare_playlist_by_bitrate);

    self->current_variant = g_list_find_custom (self->lists, top_variant_uri,
        (GCompareFunc) _m3u8_compare_uri);
  }

  /* calculate the start and end times of this media playlist. */
  if(self->files){
    GList *walk;
    GstM3U8MediaFile *file;
    GstClockTime duration = 0;

    for (walk = self->files; walk; walk = walk->next) {
      file = walk->data;
      duration += file->duration;
      if(file->sequence > client->highest_sequence_number){
        if(client->highest_sequence_number>=0){
          /* if an update of the media playlist has been missed, there
             will be a gap between self->highest_sequence_number and the
             first sequence number in this media playlist. In this situation
             assume that the missing fragments had a duration of
             targetduration each */
          client->last_file_end += (file->sequence - client->highest_sequence_number - 1) *
            self->targetduration;
        }
        client->last_file_end += file->duration;
        client->highest_sequence_number=file->sequence;
      }
    }
    if(gst_m3u8_client_is_live_no_lock (client)){
      client->first_file_start = client->last_file_end - duration;
      GST_DEBUG ("Live playlist range %" GST_TIME_FORMAT " -> %" GST_TIME_FORMAT,
                 GST_TIME_ARGS (client->first_file_start),
                 GST_TIME_ARGS (client->last_file_end));
    }
  }

  return TRUE;
}

GstM3U8Client *
gst_m3u8_client_new (const gchar * uri)
{
  GstM3U8Client *client;

  g_return_val_if_fail (uri != NULL, NULL);

  client = g_new0 (GstM3U8Client, 1);
  client->main = gst_m3u8_new ();
  client->current = NULL;
  client->sequence = -1;
  client->update_failed_count = 0;
  client->highest_sequence_number=-1;
  client->lock = g_mutex_new ();
  gst_m3u8_set_uri (client->main, g_strdup (uri));

  return client;
}

void
gst_m3u8_client_free (GstM3U8Client * self)
{
  g_return_if_fail (self != NULL);

  gst_m3u8_free (self->main);
  g_mutex_free (self->lock);
  g_free (self);
}

void
gst_m3u8_client_set_current (GstM3U8Client * self, GstM3U8 * m3u8)
{
  g_return_if_fail (self != NULL);

  GST_M3U8_CLIENT_LOCK (self);
  if (m3u8 != self->current) {
    self->current = m3u8;
    self->update_failed_count = 0;
  }
  GST_M3U8_CLIENT_UNLOCK (self);
}

gboolean
gst_m3u8_client_update (GstM3U8Client * self, gchar * data)
{
  GstM3U8 *m3u8;
  gboolean updated = FALSE;
  gboolean ret = FALSE;

  g_return_val_if_fail (self != NULL, FALSE);

  GST_M3U8_CLIENT_LOCK (self);
  m3u8 = self->current ? self->current : self->main;

  if (!gst_m3u8_update (self, m3u8, data, &updated))
    goto out;

  if (!updated) {
    self->update_failed_count++;
    goto out;
  }

  /* select the first playlist, for now */
  if (!self->current) {
    if (self->main->lists) {
      self->current = self->main->current_variant->data;
    } else {
      self->current = self->main;
    }
  }

  if (m3u8->files && self->sequence == -1) {
    if (gst_m3u8_client_is_live_no_lock (self)) {
      /* for live streams, start GST_M3U8_LIVE_MIN_FRAGMENT_DISTANCE from
         the end of the playlist. See section 6.3.3 of HLS draft */
      gint pos =
        g_list_length (m3u8->files) - GST_M3U8_LIVE_MIN_FRAGMENT_DISTANCE;
      self->sequence =
        GST_M3U8_MEDIA_FILE (g_list_nth_data (m3u8->files,
              pos >= 0 ? pos : 0))->sequence;
    } else
      self->sequence =
        GST_M3U8_MEDIA_FILE (g_list_first (m3u8->files)->data)->sequence;
    GST_DEBUG ("Setting first sequence at %d", self->sequence);
  }

  ret = TRUE;
out:
  GST_M3U8_CLIENT_UNLOCK (self);
  return ret;
}

static gboolean
_find_next (GstM3U8MediaFile * file, GstM3U8Client * client)
{
  GST_DEBUG ("Found fragment %d", file->sequence);
  if (file->sequence >= client->sequence)
    return FALSE;
  return TRUE;
}

void
gst_m3u8_client_get_current_position (GstM3U8Client * client,
    GstClockTime * timestamp)
{
  GList *l;
  GList *walk;

  l = g_list_find_custom (client->current->files, client,
      (GCompareFunc) _find_next);

  *timestamp = client->first_file_start;
  for (walk = client->current->files; walk; walk = walk->next) {
    if (walk == l)
      break;
    *timestamp += GST_M3U8_MEDIA_FILE (walk->data)->duration;
  }
}

gboolean
gst_m3u8_client_get_next_fragment (GstM3U8Client * client,
    gboolean * discontinuity, const gchar ** uri, GstClockTime * duration,
    GstClockTime * timestamp, const gchar ** key, const guint8 ** iv)
{
  GList *l;
  GstM3U8MediaFile *file;

  g_return_val_if_fail (client != NULL, FALSE);
  g_return_val_if_fail (client->current != NULL, FALSE);
  g_return_val_if_fail (discontinuity != NULL, FALSE);

  GST_M3U8_CLIENT_LOCK (client);
  GST_DEBUG ("Looking for fragment %d", client->sequence);
  l = g_list_find_custom (client->current->files, client,
      (GCompareFunc) _find_next);
  if (l == NULL) {
    GST_M3U8_CLIENT_UNLOCK (client);
    return FALSE;
  }

  gst_m3u8_client_get_current_position (client, timestamp);

  file = GST_M3U8_MEDIA_FILE (l->data);

  *discontinuity = client->sequence != file->sequence;
  client->sequence = file->sequence + 1;

  *uri = file->uri;
  *duration = file->duration;
  *key = file->key;
  *iv = file->iv;

  GST_M3U8_CLIENT_UNLOCK (client);
  return TRUE;
}

static void
_sum_duration (GstM3U8MediaFile * self, GstClockTime * duration)
{
  *duration += self->duration;
}

GstClockTime
gst_m3u8_client_get_duration (GstM3U8Client * client)
{
  GstClockTime duration = 0;

  g_return_val_if_fail (client != NULL, GST_CLOCK_TIME_NONE);

  GST_M3U8_CLIENT_LOCK (client);
  /* We can only get the duration for on-demand streams */
/*  if (!client->current->endlist) {
    GST_M3U8_CLIENT_UNLOCK (client);
    return GST_CLOCK_TIME_NONE;
  }*/

  if (client->current && client->current->files)
    g_list_foreach (client->current->files, (GFunc) _sum_duration, &duration);
  else
    duration = GST_CLOCK_TIME_NONE;
  GST_M3U8_CLIENT_UNLOCK (client);
  return duration;
}

gboolean gst_m3u8_client_get_seek_range(GstM3U8Client * client, gint64 * start, gint64 * stop)
{
  gboolean rv;
  g_return_val_if_fail (client != NULL, FALSE);
  GST_M3U8_CLIENT_LOCK (client);
  rv = gst_m3u8_client_get_seek_range_no_lock(client, start, stop);
  GST_M3U8_CLIENT_UNLOCK (client);
  return rv;
}

gboolean gst_m3u8_client_get_seek_range_no_lock(GstM3U8Client * client, gint64 * start, gint64 * stop)
{
  GstClockTime duration=0;
  GList *walk;
  GstM3U8MediaFile *file;
  guint count;
   
  g_return_val_if_fail (client != NULL, FALSE);
  
  if(client->current==NULL || client->current->files==NULL){
	return FALSE;
  }

  count = g_list_length(client->current->files);

  /* count is used to make sure the seek range is never closer than
     GST_M3U8_LIVE_MIN_FRAGMENT_DISTANCE fragments from the end of the
     playlist - see 6.3.3. "Playing the Playlist file" of the HLS draft */
  for (walk = client->current->files; walk && count>=GST_M3U8_LIVE_MIN_FRAGMENT_DISTANCE;
       walk = walk->next) {
    file = walk->data;
    --count;
    duration += file->duration;
  }

  if (duration <= 0) {
    return FALSE;
  }
  *start = client->first_file_start;
  *stop = *start + duration;
  return TRUE;
}

GstClockTime
gst_m3u8_client_get_target_duration (GstM3U8Client * client)
{
  GstClockTime duration = GST_CLOCK_TIME_NONE;

  g_return_val_if_fail (client != NULL, GST_CLOCK_TIME_NONE);

  GST_M3U8_CLIENT_LOCK (client);
  if(client->current)
    duration = client->current->targetduration;
  GST_M3U8_CLIENT_UNLOCK (client);
  return duration;
}

const gchar *
gst_m3u8_client_get_uri (GstM3U8Client * client)
{
  const gchar *uri;

  g_return_val_if_fail (client != NULL, NULL);

  GST_M3U8_CLIENT_LOCK (client);
  uri = client->main->uri;
  GST_M3U8_CLIENT_UNLOCK (client);
  return uri;
}

const gchar *
gst_m3u8_client_get_current_uri (GstM3U8Client * client)
{
  const gchar *uri;

  g_return_val_if_fail (client != NULL, NULL);

  GST_M3U8_CLIENT_LOCK (client);
  uri = client->current->uri;
  GST_M3U8_CLIENT_UNLOCK (client);
  return uri;
}

gboolean
gst_m3u8_client_has_variant_playlist (GstM3U8Client * client)
{
  gboolean ret;

  g_return_val_if_fail (client != NULL, FALSE);

  GST_M3U8_CLIENT_LOCK (client);
  ret = (client->main->lists != NULL);
  GST_M3U8_CLIENT_UNLOCK (client);
  return ret;
}

gboolean
gst_m3u8_client_is_live (GstM3U8Client * client)
{
  gboolean ret;
  GST_M3U8_CLIENT_LOCK (client);
  ret = gst_m3u8_client_is_live_no_lock (client);
  GST_M3U8_CLIENT_UNLOCK (client);
  return ret;
}

gboolean
gst_m3u8_client_is_live_no_lock (GstM3U8Client * client)
{
  gboolean ret;

  g_return_val_if_fail (client != NULL, FALSE);

  if (!client->current || client->current->endlist)
    ret = FALSE;
  else
    ret = TRUE;
  return ret;
}

GList *
gst_m3u8_client_get_playlist_for_bitrate (GstM3U8Client * client, guint bitrate)
{
  GList *list, *current_variant;

  GST_M3U8_CLIENT_LOCK (client);
  current_variant = client->main->current_variant;

  /*  Go to the highest possible bandwidth allowed */
  while (GST_M3U8 (current_variant->data)->bandwidth < bitrate) {
    list = g_list_next (current_variant);
    if (!list)
      break;
    current_variant = list;
  }

  while (GST_M3U8 (current_variant->data)->bandwidth > bitrate) {
    list = g_list_previous (current_variant);
    if (!list)
      break;
    current_variant = list;
  }
  GST_M3U8_CLIENT_UNLOCK (client);

  return current_variant;
}

gchar *
uri_join (const gchar * uri1, const gchar * uri2)
{
  gchar *uri_copy, *tmp, *ret = NULL;

  GST_DEBUG ("uri_join %s %s", uri1, uri2);
  if (gst_uri_is_valid (uri2))
    return g_strdup (uri2);

  uri_copy = g_strdup (uri1);
  if (uri2[0] != '/') {
    /* uri2 is a relative uri2 */
     tmp = g_utf8_strchr (uri_copy, -1, '?'); // look for query params
     if(tmp){
       // find last / char, ignoring query params
       tmp = g_utf8_strrchr (uri_copy, tmp-uri_copy, '/');
     }
     else{
       // find last / char in URL
       tmp = g_utf8_strrchr (uri_copy, -1, '/');
     }
    if (!tmp) {
      GST_WARNING ("Can't build a valid uri_copy");
      goto out;
    }

    *tmp = '\0';
    ret = g_strdup_printf ("%s/%s", uri_copy, uri2);
  } else {
    /* uri2 is an absolute uri2 */
    char *scheme, *hostname;

    scheme = uri_copy;
    /* find the : in <scheme>:// */
    tmp = g_utf8_strchr (uri_copy, -1, ':');
    if (!tmp) {
      GST_WARNING ("Can't build a valid uri_copy");
      goto out;
    }

    *tmp = '\0';

    /* skip :// */
    hostname = tmp + 3;

    tmp = g_utf8_strchr (hostname, -1, '/');
    if (tmp)
      *tmp = '\0';

    ret = g_strdup_printf ("%s://%s%s", scheme, hostname, uri2);
  }

out:
  g_free (uri_copy);
  return ret;
}
