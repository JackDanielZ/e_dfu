#define EFL_BETA_API_SUPPORT
#define EFL_EO_API_SUPPORT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>

#include <e.h>
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_Con.h>
#include <Eeze.h>

#include "e_mod_main.h"

#define _EET_ENTRY "config"

typedef struct
{
   E_Gadcon_Client *gcc;
   Evas_Object *o_icon;

   Ecore_File_Monitor *config_file_monitor;
} Instance;

#define PRINT _printf

static E_Module *_module = NULL;

typedef struct
{
   Eina_Stringshare *name;
   Eina_Stringshare *command;
   Instance *inst;

   Ecore_Exe *exe;
   char *notif_buf;
   unsigned int notif_id;
   unsigned int notif_buf_len;
   unsigned int notif_cur_idx;
} Image_Info;

typedef struct
{
   Eina_Stringshare *name;
   Eina_Stringshare *id;
   Eina_Stringshare *default_image;
   Eina_List *images; /* List of Image_Info */
} Device_Info;

typedef struct
{
   Eina_List *devices; /* List of Device_Info */
} Config;

static Config *_config = NULL;
static Eet_Data_Descriptor *_config_edd = NULL;

static int
_printf(const char *fmt, ...)
{
   static FILE *fp = NULL;
   char printf_buf[1024];
   va_list args;
   int printed;

   if (!fp)
     {
        char path[1024];
        sprintf(path, "%s/eezier/log", efreet_config_home_get());
        fp = fopen(path, "a");
     }

   va_start(args, fmt);
   printed = vsprintf(printf_buf, fmt, args);
   va_end(args);

   fwrite(printf_buf, 1, strlen(printf_buf), fp);
   fflush(fp);

   return printed;
}

static void
_config_eet_load()
{
   Eet_Data_Descriptor *device_edd, *image_edd;
   if (_config_edd) return;
   Eet_Data_Descriptor_Class eddc;

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Image_Info);
   image_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_BASIC(image_edd, Image_Info, "name", name, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(image_edd, Image_Info, "command", command, EET_T_STRING);

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Device_Info);
   device_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "name", name, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "id", id, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "default_image", default_image, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_LIST(device_edd, Device_Info, "images", images, image_edd);

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Config);
   _config_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_LIST(_config_edd, Config, "devices", devices, device_edd);
}

static void
_config_save()
{
   char path[1024];
   sprintf(path, "%s/eezier/config", efreet_config_home_get());
   _config_eet_load();
   Eet_File *file = eet_open(path, EET_FILE_MODE_WRITE);
   eet_data_write(file, _config_edd, _EET_ENTRY, _config, EINA_TRUE);
   eet_close(file);
}

static Eina_Bool
_mkdir(const char *dir)
{
   if (!ecore_file_exists(dir))
     {
        Eina_Bool success = ecore_file_mkdir(dir);
        if (!success)
          {
             PRINT("Cannot create a config folder \"%s\"\n", dir);
             return EINA_FALSE;
          }
     }
   return EINA_TRUE;
}

static void
_config_init(Instance *inst)
{
   char path[1024];
   Eina_List *itr, *itr2;
   Device_Info *dev;
   Image_Info *img;

   sprintf(path, "%s/eezier", efreet_config_home_get());
   if (!_mkdir(path)) return;

   _config_eet_load();
   sprintf(path, "%s/eezier/config", efreet_config_home_get());
   Eet_File *file = eet_open(path, EET_FILE_MODE_READ);
   if (!file)
     {
        PRINT("New config\n");
        dev = calloc(1, sizeof(*dev));
        img = calloc(1, sizeof(*img));
        Image_Info *img2 = calloc(1, sizeof(*img2));

        dev->name = eina_stringshare_add("Example");
        dev->id = eina_stringshare_add("1234:5678");
        dev->default_image = eina_stringshare_add("target");

        img->name = eina_stringshare_add("target");
        img->command = eina_stringshare_add("echo \"Target launched\"");
        dev->images = eina_list_append(dev->images, img);

        img2->name = eina_stringshare_add("target2");
        img2->command = eina_stringshare_add("echo \"Target2 launched\"");
        dev->images = eina_list_append(dev->images, img2);

        _config = calloc(1, sizeof(Config));
        _config->devices = eina_list_append(_config->devices, dev);
        _config_save();
     }
   else
     {
        _config = eet_data_read(file, _config_edd, _EET_ENTRY);
        eet_close(file);
     }
   EINA_LIST_FOREACH(_config->devices, itr, dev)
     {
        EINA_LIST_FOREACH(dev->images, itr2, img)
          {
             img->inst = inst;
          }
     }
}

static void
_config_shutdown()
{
   Device_Info *dev;
   EINA_LIST_FREE(_config->devices, dev)
     {
        Image_Info *img;
        eina_stringshare_del(dev->name);
        eina_stringshare_del(dev->id);
        eina_stringshare_del(dev->default_image);
        EINA_LIST_FREE(dev->images, img)
          {
             eina_stringshare_del(img->name);
             eina_stringshare_del(img->command);
             free(img);
          }
        free(dev);
     }
   free(_config);
   _config = NULL;
}

static Eina_Stringshare *
_device_id_get(const char *device)
{
   char *path = strdup(device);
   char buf[1024];
   while (*path)
     {
        char vendor[4], product[4], *slash;
        *vendor = '\0';
        *product = '\0';
        sprintf(buf, "%s/idVendor", path);
        if (access(buf, R_OK) == 0)
          {
             FILE* fp = fopen(buf, "r");
             fread(vendor, 1, sizeof(vendor), fp);
             fclose(fp);
          }
        sprintf(buf, "%s/idProduct", path);
        if (access(buf, R_OK) == 0)
          {
             FILE* fp = fopen(buf, "r");
             fread(product, 1, sizeof(product), fp);
             fclose(fp);
          }
        if (*vendor && *product)
          {
             return eina_stringshare_printf("%.4s:%.4s", vendor, product);
          }
        slash = strrchr(path, '/');
        if (slash) *slash = '\0';
     }
   return NULL;
}

static Eina_Bool
_cmd_end_cb(void *data, int type EINA_UNUSED, void *event)
{
   Ecore_Exe_Event_Del *event_info = (Ecore_Exe_Event_Del *)event;
   Ecore_Exe *exe = event_info->exe;
   Image_Info *img = ecore_exe_data_get(exe);
   if (!img || img->inst != data) return ECORE_CALLBACK_PASS_ON;
   PRINT("EXE END %p\n", img->exe);
   free(img->notif_buf);
   img->notif_buf = NULL;
   img->notif_buf_len = img->notif_cur_idx = 0;
   return ECORE_CALLBACK_DONE;
}

static void
_notification_id_update(void *d, unsigned int id)
{
   Image_Info *img = d;

   img->notif_id = id;
}

static void
_format_img_notif(Image_Info *img, const char *str)
{
   char *cr;
   unsigned int slen = strlen(str) + 1;
   if (!img->notif_buf)
     {
        img->notif_buf_len = 1024;
        img->notif_buf = malloc(img->notif_buf_len);
     }
   if (img->notif_cur_idx + slen >= img->notif_buf_len)
     {
        img->notif_buf_len *= 2;
        img->notif_buf = realloc(img->notif_buf, img->notif_buf_len);
     }
   while ((cr = strchr(str, '\r')) != NULL)
     {
        char *last_nl;
        memcpy(img->notif_buf + img->notif_cur_idx, str, cr - str);
        str = cr + 1;

        last_nl = strrchr(img->notif_buf, '\n');
        if (last_nl) img->notif_cur_idx = last_nl - img->notif_buf + 1;
        else img->notif_cur_idx = 0;
     }
   memcpy(img->notif_buf + img->notif_cur_idx, str, strlen(str));
   img->notif_cur_idx = strlen(img->notif_buf);
}

static Eina_Bool
_cmd_output_cb(void *data, int type, void *event)
{
   E_Notification_Notify n;
   char buf_icon[1024];
   char output_buf[1024];
   Ecore_Exe_Event_Data *event_data = (Ecore_Exe_Event_Data *)event;
   const char *begin = event_data->data;
   Ecore_Exe *exe = event_data->exe;
   Image_Info *img = ecore_exe_data_get(exe);
   if (!img || img->inst != data) return ECORE_CALLBACK_PASS_ON;

   snprintf(buf_icon, sizeof(buf_icon), "%s/icon.png", e_module_dir_get(_module));
   PRINT(begin);

   if (type == ECORE_EXE_EVENT_ERROR)
      sprintf(output_buf, "<color=#F00>%s</color>", begin);
   else
      sprintf(output_buf, "<color=#0F0>%s</color>", begin);
   _format_img_notif(img, output_buf);

   memset(&n, 0, sizeof(E_Notification_Notify));
   n.app_name = "eezier";
   n.timeout = 3000;
   n.replaces_id = img->notif_id;
   n.icon.icon_path = buf_icon;
   n.summary = img->name;
   n.body = img->notif_buf;
   n.urgency = E_NOTIFICATION_NOTIFY_URGENCY_CRITICAL;
   e_notification_client_send(&n, _notification_id_update, img);

   return ECORE_CALLBACK_DONE;
}

static void
_dev_cmd_invoke(Image_Info *img)
{
   if (img->exe) return;
   img->exe = ecore_exe_pipe_run(img->command, ECORE_EXE_PIPE_READ | ECORE_EXE_PIPE_ERROR, img);
   PRINT("EXE %p\n", img->exe);

   efl_wref_add(img->exe, &(img->exe));
}

static void
_udev_added_cb(const char *dev_name, Eeze_Udev_Event  event EINA_UNUSED,
             void *data EINA_UNUSED, Eeze_Udev_Watch *watch EINA_UNUSED)
{
   Eina_List *itr;
   Device_Info *dev;
   Eina_Stringshare *dev_id = _device_id_get(dev_name);
   PRINT("Device added %s\n", dev_name);
   PRINT("ID: %s\n", dev_id);
   if (!dev_id) return;
   EINA_LIST_FOREACH(_config->devices, itr, dev)
     {
        if (!strcmp(dev->id, dev_id))
          {
             Image_Info *img;
             PRINT("DEV FOUND: target to launch %s\n", dev->default_image);
             if (!dev->default_image) goto end;
             EINA_LIST_FOREACH(dev->images, itr, img)
               {
                  if (!strcmp(img->name, dev->default_image))
                    {
                       PRINT("IMG FOUND: %s\n", img->name);
                       _dev_cmd_invoke(img);
                       goto end;
                    }
               }
             goto end;
          }
     }
end:
   eina_stringshare_del(dev_id);
}

static Eo *
_icon_create(Eo *parent, const char *path, Eo **wref)
{
   Eo *ic = wref ? *wref : NULL;
   if (!ic)
     {
        ic = elm_icon_add(parent);
        elm_icon_standard_set(ic, path);
        evas_object_show(ic);
        if (wref) efl_wref_add(ic, wref);
     }
   return ic;
}

#if 0
static Eo *
_label_create(Eo *parent, const char *text, Eo **wref)
{
   Eo *label = wref ? *wref : NULL;
   if (!label)
     {
        label = elm_label_add(parent);
        evas_object_size_hint_align_set(label, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(label, 0.0, 0.0);
        evas_object_show(label);
        if (wref) efl_wref_add(label, wref);
     }
   elm_object_text_set(label, text);
   return label;
}

static Eo *
_button_create(Eo *parent, const char *text, Eo *icon, Eo **wref, Evas_Smart_Cb cb_func, void *cb_data)
{
   Eo *bt = wref ? *wref : NULL;
   if (!bt)
     {
        bt = elm_button_add(parent);
        evas_object_size_hint_align_set(bt, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(bt, 0.0, 0.0);
        evas_object_show(bt);
        if (wref) efl_wref_add(bt, wref);
        if (cb_func) evas_object_smart_callback_add(bt, "clicked", cb_func, cb_data);
     }
   elm_object_text_set(bt, text);
   elm_object_part_content_set(bt, "icon", icon);
   return bt;
}
#endif

static void
_image_selected(void *data, E_Menu *menu EINA_UNUSED, E_Menu_Item *mi)
{
   Device_Info *dev = data;
   eina_stringshare_del(dev->default_image);
   if (!strcmp(mi->label, "Disabled"))
      dev->default_image = NULL;
   else
      dev->default_image = eina_stringshare_add(mi->label);
   _config_save();
}

static void
_config_file_changed(void *data,
      Ecore_File_Monitor *em EINA_UNUSED,
      Ecore_File_Event event, const char *_path EINA_UNUSED)
{
   Instance *inst = data;
   if (event != ECORE_FILE_EVENT_MODIFIED) return;
   PRINT("Config updated\n");
   _config_shutdown();
   _config_init(inst);
}

static Instance *
_instance_create()
{
   char path[1024];
   Instance *inst = calloc(1, sizeof(Instance));

   sprintf(path, "%s/eezier", efreet_config_home_get());
   if (!_mkdir(path)) return NULL;
   sprintf(path, "%s/eezier/config", efreet_config_home_get());
   inst->config_file_monitor = ecore_file_monitor_add(path, _config_file_changed, inst);

   return inst;
}

static void
_instance_delete(Instance *inst)
{
   if (inst->o_icon) evas_object_del(inst->o_icon);

   free(inst);
}

static void
_button_cb_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Instance *inst;
   Evas_Event_Mouse_Down *ev;

   inst = data;
   ev = event_info;
   if (ev->button == 3)
     {
        Eina_List *itr, *itr2;
        Device_Info *dev;
        E_Menu *m, *m2;
        int x, y;

        m = e_menu_new();
        EINA_LIST_FOREACH(_config->devices, itr, dev)
          {
             E_Menu_Item *mi, *mi2;
             Image_Info *img;
             char lb_text[256];
             sprintf(lb_text, "%s (%s) -> %s",
                   dev->name, dev->id, dev->default_image ? dev->default_image : "Disabled");
             mi = e_menu_item_new(m);
             e_menu_item_label_set(mi, lb_text);
             m2 = e_menu_new();
             e_menu_item_submenu_set(mi, m2);
             EINA_LIST_FOREACH(dev->images, itr2, img)
               {
                  mi2 = e_menu_item_new(m2);
                  e_menu_item_label_set(mi2, img->name);
                  e_menu_item_callback_set(mi2, _image_selected, dev);
               }
             mi2 = e_menu_item_new(m2);
             e_menu_item_label_set(mi2, "Disabled");
             e_menu_item_callback_set(mi2, _image_selected, dev);
          }

        m = e_gadcon_client_util_menu_items_append(inst->gcc, m, 0);
        e_gadcon_canvas_zone_geometry_get(inst->gcc->gadcon, &x, &y, NULL, NULL);
        e_menu_activate_mouse(m,
              e_zone_current_get(),
              x + ev->output.x, y + ev->output.y, 1, 1,
              E_MENU_POP_DIRECTION_DOWN, ev->timestamp);
     }
}

static E_Gadcon_Client *
_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style)
{
   Instance *inst;
   E_Gadcon_Client *gcc;
   char buf[4096];

   inst = _instance_create();
   _config_init(inst);

   snprintf(buf, sizeof(buf), "%s/icon.png", e_module_dir_get(_module));

   inst->o_icon = _icon_create(gc->evas, buf, NULL);

   gcc = e_gadcon_client_new(gc, name, id, style, inst->o_icon);
   gcc->data = inst;
   inst->gcc = gcc;

   evas_object_event_callback_add(inst->o_icon, EVAS_CALLBACK_MOUSE_DOWN,
				  _button_cb_mouse_down, inst);

   eeze_udev_watch_add(EEZE_UDEV_TYPE_NONE, EEZE_UDEV_EVENT_ADD, _udev_added_cb, NULL);

   ecore_event_handler_add(ECORE_EXE_EVENT_DATA, _cmd_output_cb, inst);
   ecore_event_handler_add(ECORE_EXE_EVENT_ERROR, _cmd_output_cb, inst);
   ecore_event_handler_add(ECORE_EXE_EVENT_DEL, _cmd_end_cb, inst);

   return gcc;
}

static void
_gc_shutdown(E_Gadcon_Client *gcc)
{
   _instance_delete(gcc->data);
   _config_shutdown();
}

static void
_gc_orient(E_Gadcon_Client *gcc, E_Gadcon_Orient orient EINA_UNUSED)
{
   e_gadcon_client_aspect_set(gcc, 32, 16);
   e_gadcon_client_min_size_set(gcc, 32, 16);
}

static const char *
_gc_label(const E_Gadcon_Client_Class *client_class EINA_UNUSED)
{
   return "eezier";
}

static Evas_Object *
_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas)
{
   char buf[4096];

   if (!_module) return NULL;

   snprintf(buf, sizeof(buf), "%s/icon.png", e_module_dir_get(_module));

   return _icon_create(evas, buf, NULL);
}

static const char *
_gc_id_new(const E_Gadcon_Client_Class *client_class)
{
   char buf[32];
   static int id = 0;
   sprintf(buf, "%s.%d", client_class->name, ++id);
   return eina_stringshare_add(buf);
}

EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION, "eezier"
};

static const E_Gadcon_Client_Class _gc_class =
{
   GADCON_CLIENT_CLASS_VERSION, "eezier",
   {
      _gc_init, _gc_shutdown, _gc_orient, _gc_label, _gc_icon, _gc_id_new, NULL, NULL
   },
   E_GADCON_CLIENT_STYLE_PLAIN
};

EAPI void *
e_modapi_init(E_Module *m)
{
   ecore_init();
   ecore_con_init();
   ecore_con_url_init();
   efreet_init();
   eeze_init();

   _module = m;
   e_gadcon_provider_register(&_gc_class);

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
   e_gadcon_provider_unregister(&_gc_class);

   _module = NULL;
   eeze_shutdown();
   efreet_shutdown();
   ecore_con_url_shutdown();
   ecore_con_shutdown();
   ecore_shutdown();
   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   return 1;
}
