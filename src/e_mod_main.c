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
   E_Gadcon_Popup *popup;
   Evas_Object *o_icon;
   Eo *main_box;

   Eina_List *items;
   Ecore_File_Monitor *config_dir_monitor;
   Eina_Stringshare *cfg_path;

   int fd;
} Instance;

#define PRINT printf

static E_Module *_module = NULL;

typedef struct
{
   Instance *instance;
   Ecore_Timer *timer;
   Eina_Stringshare *filename;
   Eina_Stringshare *name;
   Eo *start_button;
   Eina_Bool playing;
   char *filedata;
   char *cur_filedata;
   char *cur_state;
} Item_Desc;

typedef struct
{
   Eina_Stringshare *name;
   Eina_Stringshare *command;
} Image_Info;

typedef struct
{
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

static void _start_stop_bt_clicked(void *data, Evas_Object *obj, void *event_info);

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
   sprintf(path, "%s/e_dfu/config", efreet_config_home_get());
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
_config_init()
{
   char path[1024];

   sprintf(path, "%s/e_dfu", efreet_config_home_get());
   if (!_mkdir(path)) return;

   _config_eet_load();
   sprintf(path, "%s/e_dfu/config", efreet_config_home_get());
   Eet_File *file = eet_open(path, EET_FILE_MODE_READ);
   if (!file)
     {
        PRINT("DFU new config\n");
        Device_Info *dev = calloc(1, sizeof(*dev));
        Image_Info *img = calloc(1, sizeof(*img));
        img->name = eina_stringshare_add("target");
        img->command = eina_stringshare_add("echo \"Target launched\"");
        dev->id = eina_stringshare_add("1234:5678");
        dev->default_image = eina_stringshare_add("target");
        dev->images = eina_list_append(dev->images, img);
        _config = calloc(1, sizeof(Config));
        _config->devices = eina_list_append(_config->devices, dev);
        _config_save();
     }
   else
     {
        _config = eet_data_read(file, _config_edd, _EET_ENTRY);
        eet_close(file);
     }
}

static void
_config_shutdown()
{
   Device_Info *dev;
   EINA_LIST_FREE(_config->devices, dev)
     {
        Image_Info *img;
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

static void
_udev_added_cb(const char *device, Eeze_Udev_Event  event EINA_UNUSED,
             void *data EINA_UNUSED, Eeze_Udev_Watch *watch EINA_UNUSED)
{
   PRINT("Device added %s\n", device);
   PRINT("ID: %s\n", _device_id_get(device));
}

#if 0
static Eo *
_label_create(Eo *parent, const char *text, Eo **wref)
{
   Eo *label = wref ? *wref : NULL;
   if (!label)
     {
        label = elm_label_add(parent);
        evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, 0.0);
        evas_object_show(label);
        if (wref) efl_wref_add(label, wref);
     }
   elm_object_text_set(label, text);
   return label;
}
#endif

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

static char *
_file_get_as_string(const char *filename)
{
   char *file_data = NULL;
   int file_size;
   FILE *fp = fopen(filename, "rb");
   if (!fp)
     {
        PRINT("Can not open file: \"%s\".", filename);
        return NULL;
     }

   fseek(fp, 0, SEEK_END);
   file_size = ftell(fp);
   if (file_size == -1)
     {
        fclose(fp);
        PRINT("Can not ftell file: \"%s\".", filename);
        return NULL;
     }
   rewind(fp);
   file_data = (char *) calloc(1, file_size + 1);
   if (!file_data)
     {
        fclose(fp);
        PRINT("Calloc failed");
        return NULL;
     }
   int res = fread(file_data, file_size, 1, fp);
   fclose(fp);
   if (!res)
     {
        free(file_data);
        file_data = NULL;
        PRINT("fread failed");
     }
   return file_data;
}

static void
_start_stop_bt_clicked(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   Eina_List *itr;
   Item_Desc *idesc = data, *idesc2;
   idesc->playing = !idesc->playing;
   elm_object_part_content_set(idesc->start_button, "icon",
      _icon_create(idesc->start_button,
         idesc->playing ? "media-playback-stop" : "media-playback-start", NULL));
   if (idesc->playing)
     {
        idesc->filedata = _file_get_as_string(idesc->filename);
        idesc->cur_filedata = idesc->filedata;
        idesc->cur_state = NULL;
        PRINT("Beginning consuming %s", idesc->filename);
     }
   else
     {
        free(idesc->filedata);
        idesc->filedata = idesc->cur_filedata = NULL;
        ecore_timer_del(idesc->timer);
        idesc->timer = NULL;
     }
   EINA_LIST_FOREACH(idesc->instance->items, itr, idesc2)
     {
        if (idesc2 != idesc)
           elm_object_disabled_set(idesc2->start_button, idesc->playing);
     }
}

static void
_box_update(Instance *inst, Eina_Bool clear)
{
   Eina_List *itr;
   Item_Desc *idesc;

   if (!inst->main_box) return;

   if (clear) elm_box_clear(inst->main_box);

   EINA_LIST_FOREACH(inst->items, itr, idesc)
     {
        Eo *b = elm_box_add(inst->main_box);
        elm_box_horizontal_set(b, EINA_TRUE);
        evas_object_size_hint_align_set(b, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(b, EVAS_HINT_EXPAND, 0.0);
        evas_object_show(b);
        elm_box_pack_end(inst->main_box, b);

        _button_create(b, idesc->name, NULL,
              &idesc->start_button, _start_stop_bt_clicked, idesc);
        evas_object_size_hint_weight_set(idesc->start_button, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
        elm_box_pack_end(b, idesc->start_button);
     }
}

static void
_config_dir_changed(void *data EINA_UNUSED,
      Ecore_File_Monitor *em EINA_UNUSED,
      Ecore_File_Event event EINA_UNUSED, const char *_path EINA_UNUSED)
{
   PRINT("e_dfu: config updated\n");
   _config_shutdown();
   _config_init();
}

static Instance *
_instance_create()
{
   char path[1024];
   Instance *inst = calloc(1, sizeof(Instance));

   sprintf(path, "%s/e_dfu", efreet_config_home_get());
   if (!_mkdir(path)) return NULL;
   inst->cfg_path = eina_stringshare_add(path);
   inst->config_dir_monitor = ecore_file_monitor_add(path, _config_dir_changed, inst);

     {
//        free(inst);
//        inst = NULL;
     }
   return inst;
}

static void
_instance_delete(Instance *inst)
{
   if (inst->o_icon) evas_object_del(inst->o_icon);
   if (inst->main_box) evas_object_del(inst->main_box);

   free(inst);
}

static void
_popup_del(Instance *inst)
{
   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_popup_del_cb(void *obj)
{
   _popup_del(e_object_data_get(obj));
}

static void
_popup_comp_del_cb(void *data, Evas_Object *obj EINA_UNUSED)
{
   Instance *inst = data;

   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_button_cb_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Instance *inst;
   Evas_Event_Mouse_Down *ev;

   inst = data;
   ev = event_info;
   if (ev->button == 1)
     {
        if (!inst->popup)
          {
             Evas_Object *o;
             inst->popup = e_gadcon_popup_new(inst->gcc, 0);

             o = elm_box_add(e_comp->elm);
             evas_object_size_hint_align_set(o, EVAS_HINT_FILL, EVAS_HINT_FILL);
             evas_object_size_hint_weight_set(o, EVAS_HINT_EXPAND, 0.0);
             evas_object_show(o);
             efl_wref_add(o, &inst->main_box);

             _box_update(inst, EINA_FALSE);

             e_gadcon_popup_content_set(inst->popup, inst->main_box);
             e_comp_object_util_autoclose(inst->popup->comp_object,
                   _popup_comp_del_cb, NULL, inst);
             e_gadcon_popup_show(inst->popup);
             e_object_data_set(E_OBJECT(inst->popup), inst);
             E_OBJECT_DEL_SET(inst->popup, _popup_del_cb);
          }
     }
}

static E_Gadcon_Client *
_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style)
{
   Instance *inst;
   E_Gadcon_Client *gcc;
   char buf[4096];

   _config_init();
   inst = _instance_create();

   snprintf(buf, sizeof(buf), "%s/dfu.edj", e_module_dir_get(_module));

   inst->o_icon = edje_object_add(gc->evas);
   if (!e_theme_edje_object_set(inst->o_icon,
				"base/theme/modules/dfu",
                                "modules/dfu/main"))
      edje_object_file_set(inst->o_icon, buf, "modules/dfu/main");
   evas_object_show(inst->o_icon);

   gcc = e_gadcon_client_new(gc, name, id, style, inst->o_icon);
   gcc->data = inst;
   inst->gcc = gcc;

   evas_object_event_callback_add(inst->o_icon, EVAS_CALLBACK_MOUSE_DOWN,
				  _button_cb_mouse_down, inst);

   eeze_udev_watch_add(EEZE_UDEV_TYPE_NONE, EEZE_UDEV_EVENT_ADD, _udev_added_cb, NULL);

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
   return "DFU";
}

static Evas_Object *
_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas)
{
   Evas_Object *o;
   char buf[4096];

   if (!_module) return NULL;

   snprintf(buf, sizeof(buf), "%s/e-module-dfu.edj", e_module_dir_get(_module));

   o = edje_object_add(evas);
   edje_object_file_set(o, buf, "icon");
   return o;
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
   E_MODULE_API_VERSION, "DFU"
};

static const E_Gadcon_Client_Class _gc_class =
{
   GADCON_CLIENT_CLASS_VERSION, "dfu",
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
