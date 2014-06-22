#ifndef _GRL_AMPACHE_SOURCE_H_
#define _GRL_AMPACHE_SOURCE_H_

#include <grilo.h>

#define GRL_AMPACHE_SOURCE_TYPE                         (grl_ampache_source_get_type())
#define GRL_IS_AMPACHE_SOURCE(obj)                      (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GRL_AMPACHE_SOURCE_TYPE))
#define GRL_AMPACHE_SOURCE(obj)                         (G_TYPE_CHECK_INSTANCE_CAST((obj), GRL_AMPACHE_SOURCE_TYPE, GrlAmpacheSource))
#define GRL_AMPACHE_SOURCE_CLASS(klass)                 (G_TYPE_CHECK_CLASS_CAST((klass), GRL_AMPACHE_SOURCE_TYPE, GrlAmpacheSourceClass))
#define GRL_IS_AMPACHE_SOURCE_CLASS(klass)              (G_TYPE_CHECK_CLASS_TYPE((klass), GRL_AMPACHE_SOURCE_TYPE))
#define GRL_AMPACHE_SOURCE_GET_CLASS(obj)               (G_TYPE_INSTANCE_GET_CLASS((obj), GRL_AMPACHE_SOURCE_TYPE, GrlAmpacheSourceClass))

typedef struct _GrlAmpacheSource GrlAmpacheSource;

typedef struct _GrlAmpacheSourcePriv GrlAmpacheSourcePriv;

struct _GrlAmpacheSource {
  GrlSource parent;
  GrlAmpacheSourcePriv *priv;
};

typedef struct _GrlAmpacheSourceClass GrlAmpacheSourceClass;

struct _GrlAmpacheSourceClass {
  GrlSourceClass parent_class;
};

GType grl_ampache_source_get_type (void);

#endif /* _GRL_AMPACHE_SOURCE_H_ */
