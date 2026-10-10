#include "lifetime.h"

static void
on_activate (GtkApplication *app, void *data G_GNUC_UNUSED)
{
  GtkWidget *window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "gbk-file");
  gtk_window_set_default_size (GTK_WINDOW (window), 700, 440);
  gtk_widget_show_all (window);
}

int
main (int argc, char **argv)
{
  if (argc == 2 && g_str_equal (argv[1], "--version"))
    {
      printf ("gbk-file %s\n", GBK_VERSION);
      return 0;
    }

  GtkApplication *app = gtk_application_new ("net.gbk.File", G_APPLICATION_DEFAULT_FLAGS);
  gbk_connect (G_OBJECT (app), app, "activate", G_CALLBACK (on_activate), nullptr);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  /* Everything has been torn down; any registration still alive is a lifetime bug. */
  if (gbk_lifetime_live () != 0)
    g_error ("%d signal connection(s)/source(s) outlived their owners", gbk_lifetime_live ());
  return status;
}
