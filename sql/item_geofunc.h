#ifndef ITEM_GEOFUNC_INCLUDED
#define ITEM_GEOFUNC_INCLUDED

/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */


/* This file defines all spatial functions */
#include "inplace_vector.h"
#include "item_cmpfunc.h"      // Item_bool_func2
#include "prealloced_array.h"
#include "spatial.h"           // gis_wkb_raw_free
#include "item_strfunc.h"      // Item_str_func
#include "item_json_func.h"    // Item_json_func

#include <vector>

class Json_dom;
class Json_array;
class Json_object;
enum class enum_json_type;

/**
   We have to hold result buffers in functions that return a GEOMETRY string,
   because such a function's result geometry's buffer is directly used and
   set to String result object. We have to release them properly manually
   since they won't be released when the String result is destroyed.
*/
class BG_result_buf_mgr
{
  typedef Prealloced_array<void *, 64> Prealloced_buffers;
public:
  BG_result_buf_mgr() :bg_result_buf(NULL), bg_results(PSI_INSTRUMENT_ME)
  {
  }

  ~BG_result_buf_mgr()
  {
    free_intermediate_result_buffers();
    free_result_buffer();
  }

  void add_buffer(void *buf)
  {
    bg_results.insert_unique(buf);
  }


  void forget_buffer(void *buf)
  {
    if (bg_result_buf == buf)
      bg_result_buf= NULL;
    bg_results.erase_unique(buf);
  }


  /* Free intermediate result buffers accumulated during GIS calculation. */
  void free_intermediate_result_buffers()
  {
    bg_results.erase_unique(bg_result_buf);
    for (Prealloced_buffers::iterator itr= bg_results.begin();
         itr != bg_results.end(); ++itr)
      gis_wkb_raw_free(*itr);
    bg_results.clear();
  }


  // Free the final result buffer, should be called after the result used.
  void free_result_buffer()
  {
    gis_wkb_raw_free(bg_result_buf);
    bg_result_buf= NULL;
  }


  void set_result_buffer(void *buf)
  {
    bg_result_buf= buf;
    bg_results.erase_unique(bg_result_buf);
  }

private:
  /*
    Hold data buffer of this set operation's final result geometry which is
    freed next time val_str is called since it can be used by upper Item nodes.
  */
  void *bg_result_buf;

  /*
    Result buffers for intermediate set operation results, which are freed
    before val_str returns.
  */
  Prealloced_buffers bg_results;
};


class Item_func_spatial_operation;

/**
  A utility class to flatten any hierarchy of geometry collection into one
  with no nested geometry collections. All components are stored separately
  and all their data stored in this class, in order to easily manipulate them.
 */
class BG_geometry_collection
{
  bool comp_no_overlapped;
  Geometry::srid_t m_srid;
  size_t m_num_isolated;
  std::vector<Geometry*> m_geos;
  Inplace_vector<Geometry_buffer> m_geobufs;
  Inplace_vector<String> m_geosdata;
public:
  typedef std::vector<Geometry *> Geometry_list;

  BG_geometry_collection();

  bool is_comp_no_overlapped() const
  {
    return comp_no_overlapped;
  }

  void set_comp_no_overlapped(bool b)
  {
    comp_no_overlapped= b;
  }

  Geometry::srid_t get_srid() const
  {
    return m_srid;
  }

  void set_srid(Geometry::srid_t srid)
  {
    m_srid= srid;
  }

  bool fill(const Geometry *geo, bool break_multi_geom= false)
  {
    return store_geometry(geo, break_multi_geom);
  }

  const Geometry_list &get_geometries() const
  {
    return m_geos;
  }

  Geometry_list &get_geometries()
  {
    return m_geos;
  }

  bool all_isolated() const
  {
    return m_num_isolated == m_geos.size();
  }

  size_t num_isolated() const
  {
    return m_num_isolated;
  }

  Gis_geometry_collection *as_geometry_collection(String *geodata) const;
  template<typename Coordsys>
  void merge_components(my_bool *pnull_value);
private:
  template<typename Coordsys>
  bool merge_one_run(Item_func_spatial_operation *ifso,
                     my_bool *pnull_value);
  bool store_geometry(const Geometry *geo, bool break_multi_geom);
  Geometry *store(const Geometry *geo);
};


class Item_geometry_func: public Item_str_func
{
public:
  Item_geometry_func() :Item_str_func() {}

  Item_geometry_func(Item *a) :Item_str_func(a) {}
  Item_geometry_func(const POS &pos, Item *a) :Item_str_func(pos, a) {}

  Item_geometry_func(Item *a,Item *b) :Item_str_func(a,b) {}
  Item_geometry_func(const POS &pos, Item *a,Item *b) :Item_str_func(pos, a,b) {}

  Item_geometry_func(Item *a,Item *b,Item *c) :Item_str_func(a,b,c) {}
  Item_geometry_func(const POS &pos, Item *a, Item *b, Item *c)
    :Item_str_func(pos, a, b, c)
  {}
  Item_geometry_func(const POS &pos, PT_item_list *list);

  virtual bool resolve_type(THD *thd);
  enum_field_types field_type() const  { return MYSQL_TYPE_GEOMETRY; }
  Field *tmp_table_field(TABLE *t_arg);
};

class Item_func_geometry_from_text: public Item_geometry_func
{
public:
  enum class Functype
  {
    GEOMCOLLFROMTEXT,
    GEOMCOLLFROMTXT,
    GEOMETRYCOLLECTIONFROMTEXT,
    GEOMETRYFROMTEXT,
    GEOMFROMTEXT,
    LINEFROMTEXT,
    LINESTRINGFROMTEXT,
    MLINEFROMTEXT,
    MPOINTFROMTEXT,
    MPOLYFROMTEXT,
    MULTILINESTRINGFROMTEXT,
    MULTIPOINTFROMTEXT,
    MULTIPOLYGONFROMTEXT,
    POINTFROMTEXT,
    POLYFROMTEXT,
    POLYGONFROMTEXT
  };

private:
  typedef Item_geometry_func super;
  Functype m_functype;
  /**
    Get the type of geometry that this Item can return.

    @return The geometry type
  */
  Geometry::wkbType allowed_wkb_type() const;
  /**
    Check if a geometry type is a valid return type for this Item.

    @param type The type to check

    @retval true The geometry type is allowed
    @retval false The geometry type is not allowed
  */
  bool is_allowed_wkb_type(Geometry::wkbType type) const;

public:
  Item_func_geometry_from_text(const POS &pos, Item *a, Functype functype)
    :Item_geometry_func(pos, a), m_functype(functype)
  {}
  Item_func_geometry_from_text(const POS &pos, Item *a, Item *srid,
                               Functype functype)
    :Item_geometry_func(pos, a, srid), m_functype(functype)
  {}

  virtual bool itemize(Parse_context *pc, Item **res);
  const char *func_name() const;
  String *val_str(String *);
};

class Item_func_geometry_from_wkb: public Item_geometry_func
{
public:
  enum class Functype
  {
    GEOMCOLLFROMWKB,
    GEOMETRYCOLLECTIONFROMWKB,
    GEOMETRYFROMWKB,
    GEOMFROMWKB,
    LINEFROMWKB,
    LINESTRINGFROMWKB,
    MLINEFROMWKB,
    MPOINTFROMWKB,
    MPOLYFROMWKB,
    MULTILINESTRINGFROMWKB,
    MULTIPOINTFROMWKB,
    MULTIPOLYGONFROMWKB,
    POINTFROMWKB,
    POLYFROMWKB,
    POLYGONFROMWKB
  };

private:
  typedef Item_geometry_func super;
  String tmp_value;
  Functype m_functype;
  /**
    Get the type of geometry that this Item can return.

    @return The geometry type
  */
  Geometry::wkbType allowed_wkb_type() const;
  /**
    Check if a geometry type is a valid return type for this Item.

    @param type The type to check

    @retval true The geometry type is allowed
    @retval false The geometry type is not allowed
  */
  bool is_allowed_wkb_type(Geometry::wkbType type) const;

public:
  Item_func_geometry_from_wkb(const POS &pos, Item *a, Functype functype)
    : Item_geometry_func(pos, a), m_functype(functype)
  { }
  Item_func_geometry_from_wkb(const POS &pos, Item *a, Item *srid,
                              Functype functype)
    : Item_geometry_func(pos, a, srid), m_functype(functype)
  { }

  virtual bool itemize(Parse_context *pc, Item **res);
  const char *func_name() const;
  String *val_str(String *);
};

class Item_func_as_wkt: public Item_str_ascii_func
{
public:
  Item_func_as_wkt(const POS &pos, Item *a): Item_str_ascii_func(pos, a) {}
  const char *func_name() const { return "st_astext"; }
  String *val_str_ascii(String *);
  virtual bool resolve_type(THD *thd);
};

class Item_func_as_wkb: public Item_geometry_func
{
public:
  Item_func_as_wkb(const POS &pos, Item *a): Item_geometry_func(pos, a) {}
  const char *func_name() const { return "st_aswkb"; }
  String *val_str(String *);
  enum_field_types field_type() const  { return MYSQL_TYPE_BLOB; }
};

class Item_func_geometry_type: public Item_str_ascii_func
{
public:
  Item_func_geometry_type(const POS &pos, Item *a): Item_str_ascii_func(pos, a)
  {}
  String *val_str_ascii(String *);
  const char *func_name() const { return "st_geometrytype"; }
  virtual bool resolve_type(THD *thd)
  {
    // "GeometryCollection" is the longest
    fix_length_and_charset(20, default_charset());
    maybe_null= true;
    return false;
  };
};


/**
  This handles one function:

    @<geometry@> = ST_GEOMFROMGEOJSON(@<string@>[, @<options@>[, @<srid@>]])

  Options is an integer argument which determines how positions with higher
  coordinate dimension than MySQL support should be handled. The function will
  accept both single objects, geometry collections and feature objects and
  collections. All "properties" members of GeoJSON feature objects is ignored.

  The implementation conforms with GeoJSON revision 1.0 described at
  http://geojson.org/geojson-spec.html.
*/
class Item_func_geomfromgeojson : public Item_geometry_func
{
public:
  /**
    Describing how coordinate dimensions higher than supported in MySQL
    should be handled.
  */
  enum enum_handle_coordinate_dimension
  {
    reject_document, strip_now_accept_future, strip_now_reject_future,
    strip_now_strip_future
  };
  Item_func_geomfromgeojson(const POS &pos, Item *json_string)
    :Item_geometry_func(pos, json_string),
    m_handle_coordinate_dimension(reject_document), m_user_provided_srid(false),
    m_srid_found_in_document(-1)
  {}
  Item_func_geomfromgeojson(const POS &pos, Item *json_string, Item *options)
    :Item_geometry_func(pos, json_string, options), m_user_provided_srid(false),
    m_srid_found_in_document(-1)
  {}
  Item_func_geomfromgeojson(const POS &pos, Item *json_string, Item *options,
                            Item *srid)
    :Item_geometry_func(pos, json_string, options, srid),
    m_srid_found_in_document(-1)
  {}
  String *val_str(String *);
  bool fix_fields(THD *, Item **ref);
  const char *func_name() const { return "st_geomfromgeojson"; }
  Geometry::wkbType get_wkbtype(const char *typestring);
  bool get_positions(const Json_array *coordinates, Gis_point *point);
  bool get_linestring(const Json_array *data_array,
                      Gis_line_string *linestring);
  bool get_polygon(const Json_array *data_array, Gis_polygon *polygon);
  bool parse_object(const Json_object *object, bool *rollback,
                    String *buffer, bool is_parent_featurecollection,
                    Geometry **geometry);
  bool parse_object_array(const Json_array *points,
                          Geometry::wkbType type, bool *rollback,
                          String *buffer, bool is_parent_featurecollection,
                          Geometry **geometry);
  static bool check_argument_valid_integer(Item *argument);
  bool parse_crs_object(const Json_object *crs_object);
  bool is_member_valid(const Json_dom *member, const char *member_name,
                       enum_json_type expected_type, bool allow_null,
                       bool *was_null);
  const Json_dom *
  my_find_member_ncase(const Json_object *object, const char *member_name);

  static const char *TYPE_MEMBER;
  static const char *CRS_MEMBER;
  static const char *GEOMETRY_MEMBER;
  static const char *PROPERTIES_MEMBER;
  static const char *FEATURES_MEMBER;
  static const char *GEOMETRIES_MEMBER;
  static const char *COORDINATES_MEMBER;
  static const char *CRS_NAME_MEMBER;
  static const char *NAMED_CRS;
  static const char *SHORT_EPSG_PREFIX;
  static const char *LONG_EPSG_PREFIX;
  static const char *CRS84_URN;
  static const char *POINT_TYPE;
  static const char *MULTIPOINT_TYPE;
  static const char *LINESTRING_TYPE;
  static const char *MULTILINESTRING_TYPE;
  static const char *POLYGON_TYPE;
  static const char *MULTIPOLYGON_TYPE;
  static const char *GEOMETRYCOLLECTION_TYPE;
  static const char *FEATURE_TYPE;
  static const char *FEATURECOLLECTION_TYPE;
private:
  /**
    How higher coordinate dimensions than currently supported should be handled.
  */
  enum_handle_coordinate_dimension m_handle_coordinate_dimension;
  /// Is set to true if user provided a SRID as an argument.
  bool m_user_provided_srid;
  /// The SRID user provided as an argument.
  Geometry::srid_t m_user_srid;
  /**
    The SRID value of the document CRS, if one is found. Otherwise, this value
    defaults to -1.
  */
  longlong m_srid_found_in_document;
};


/// Max width of long CRS URN supported + max width of SRID + '\0'.
static const int MAX_CRS_WIDTH= (22 + MAX_INT_WIDTH + 1);

/**
  This class handles the following function:

  @<json@> = ST_ASGEOJSON(@<geometry@>[, @<maxdecimaldigits@>[, @<options@>]])

  It converts a GEOMETRY into a valid GeoJSON string. If maxdecimaldigits is
  specified, the coordinates written are rounded to the number of decimals
  specified (e.g with decimaldigits = 3: 10.12399 => 10.124).

  Options is a bitmask with the following flags:
  0  No options (default values).
  1  Add a bounding box to the output.
  2  Add a short CRS URN to the output. The default format is a
     short format ("EPSG:<srid>").
  4  Add a long format CRS URN ("urn:ogc:def:crs:EPSG::<srid>"). This
     implies 2. This means that, e.g., bitmask 5 and 7 mean the
     same: add a bounding box and a long format CRS URN.
*/
class Item_func_as_geojson :public Item_json_func
{
private:
  /// Maximum number of decimal digits in printed coordinates.
  int m_max_decimal_digits;
  /// If true, the output GeoJSON has a bounding box for each GEOMETRY.
  bool m_add_bounding_box;
  /**
    If true, the output GeoJSON has a CRS object in the short
    form (e.g "EPSG:4326").
  */
  bool m_add_short_crs_urn;
  /**
    If true, the output GeoJSON has a CRS object in the long
    form (e.g "urn:ogc:def:crs:EPSG::4326").
  */
  bool m_add_long_crs_urn;
  /// The SRID found in the input GEOMETRY.
  uint32 m_geometry_srid;
public:
  Item_func_as_geojson(THD *thd, const POS &pos, Item *geometry)
    :Item_json_func(thd, pos, geometry), m_add_bounding_box(false),
    m_add_short_crs_urn(false), m_add_long_crs_urn(false)
  {}
  Item_func_as_geojson(THD *thd, const POS &pos, Item *geometry, Item *maxdecimaldigits)
    :Item_json_func(thd, pos, geometry, maxdecimaldigits),
    m_add_bounding_box(false), m_add_short_crs_urn(false),
    m_add_long_crs_urn(false)
  {}
  Item_func_as_geojson(THD *thd, const POS &pos, Item *geometry, Item *maxdecimaldigits,
                       Item *options)
    :Item_json_func(thd, pos, geometry, maxdecimaldigits, options),
    m_add_bounding_box(false), m_add_short_crs_urn(false),
    m_add_long_crs_urn(false)
  {}
  bool fix_fields(THD *thd, Item **ref);
  bool val_json(Json_wrapper *wr);
  const char *func_name() const { return "st_asgeojson"; }
  bool parse_options_argument();
  bool parse_maxdecimaldigits_argument();
};


/**
  This class handles two forms of the same function:

  @<string@> = ST_GEOHASH(@<point@>, @<maxlength@>);
  @<string@> = ST_GEOHASH(@<longitude@>, @<latitude@>, @<maxlength@>)

  It returns an encoded geohash string, no longer than @<maxlength@> characters
  long. Note that it might be shorter than @<maxlength@>.
*/
class Item_func_geohash :public Item_str_ascii_func
{
private:
  /// The latitude argument supplied by the user (directly or by a POINT).
  double latitude;
  /// The longitude argument supplied by the user (directly or by a POINT).
  double longitude;
  /// The maximum output length of the geohash, supplied by the user.
  uint geohash_max_output_length;

  /**
    The maximum input latitude. For now, this is set to 90.0. It can be
    changed to support a different range than the normal [90, -90].
  */
  const double max_latitude;

  /**
    The minimum input latitude. For now, this is set to -90.0. It can be
    changed to support a different range than the normal [90, -90].
  */
  const double min_latitude;

  /**
    The maximum input longitude. For now, this is set to 180.0. It can be
    changed to support a different range than the normal [180, -180].
  */
  const double max_longitude;

  /**
    The minimum input longitude. For now, this is set to -180.0. It can be
    changed to support a different range than the normal [180, -180].
  */
  const double min_longitude;

  /**
    The absolute upper limit of geohash output length. User will get an error
    if they supply a max geohash length argument greater than this.
  */
  const uint upper_limit_output_length;
public:
  Item_func_geohash(const POS &pos, Item *point, Item *length)
    :Item_str_ascii_func(pos, point, length), max_latitude(90.0),
    min_latitude(-90.0), max_longitude(180.0), min_longitude(-180.0),
    upper_limit_output_length(100)
  {}
  Item_func_geohash(const POS &pos, Item *longitude, Item *latitude,
                    Item *length)
    :Item_str_ascii_func(pos, longitude, latitude, length), max_latitude(90.0),
    min_latitude(-90.0), max_longitude(180.0), min_longitude(-180.0),
    upper_limit_output_length(100)
  {}
  String *val_str_ascii(String *);
  virtual bool resolve_type(THD *thd);
  bool fix_fields(THD *thd, Item **ref);
  const char *func_name() const { return "st_geohash"; }
  char char_to_base32(char char_input);
  void encode_bit(double *upper_value, double *lower_value,
                  double target_value, char *char_value, int bit_number);
  bool fill_and_check_fields();
  bool check_valid_latlong_type(Item *ref);
};


/**
  This is a superclass for Item_func_longfromgeohash and
  Item_func_latfromgeohash, since they share almost all code.
*/
class Item_func_latlongfromgeohash :public Item_real_func
{
private:
  /**
   The lower limit for latitude output value. Normally, this will be
   set to -90.0.
  */
  const double lower_latitude;

  /**
   The upper limit for latitude output value. Normally, this will be
   set to 90.0.
  */
  const double upper_latitude;

  /**
   The lower limit for longitude output value. Normally, this will
   be set to -180.0.
  */
  const double lower_longitude;

  /**
   The upper limit for longitude output value. Normally, this will
   be set to 180.0.
  */
  const double upper_longitude;

  /**
   If this is set to TRUE the algorithm will start decoding on the first bit,
   which decodes a longitude value. If it is FALSE, it will start on the
   second bit which decodes a latitude value.
  */
  const bool start_on_even_bit;
public:
  Item_func_latlongfromgeohash(const POS &pos, Item *a,
                               double lower_latitude, double upper_latitude,
                               double lower_longitude, double upper_longitude,
                               bool start_on_even_bit_arg)
    :Item_real_func(pos, a), lower_latitude(lower_latitude),
    upper_latitude(upper_latitude), lower_longitude(lower_longitude),
    upper_longitude(upper_longitude), start_on_even_bit(start_on_even_bit_arg)
  {}
  double val_real();
  virtual bool resolve_type(THD *thd);
  bool fix_fields(THD *thd, Item **ref);
  static bool decode_geohash(String *geohash, double upper_latitude,
                             double lower_latitude, double upper_longitude,
                             double lower_longitude, double *result_latitude,
                             double *result_longitude);
  static double round_latlongitude(double latlongitude, double error_range,
                                   double lower_limit, double upper_limit);
  static bool check_geohash_argument_valid_type(Item *item);
};


/**
  This handles the @<double@> = ST_LATFROMGEOHASH(@<string@>) function.
  It returns the latitude-part of a geohash, in the range of [-90, 90].
*/
class Item_func_latfromgeohash :public Item_func_latlongfromgeohash
{
public:
  Item_func_latfromgeohash(const POS &pos, Item *a)
    :Item_func_latlongfromgeohash(pos, a, -90.0, 90.0, -180.0, 180.0, false)
  {}

  const char *func_name() const { return "ST_LATFROMGEOHASH"; }
};


/**
  This handles the @<double@> = ST_LONGFROMGEOHASH(@<string@>) function.
  It returns the longitude-part of a geohash, in the range of [-180, 180].
*/
class Item_func_longfromgeohash :public Item_func_latlongfromgeohash
{
public:
  Item_func_longfromgeohash(const POS &pos, Item *a)
    :Item_func_latlongfromgeohash(pos, a, -90.0, 90.0, -180.0, 180.0, true)
  {}

  const char *func_name() const { return "ST_LONGFROMGEOHASH"; }
};


class Item_func_centroid: public Item_geometry_func
{
  BG_result_buf_mgr bg_resbuf_mgr;

  template <typename Coordsys>
  bool bg_centroid(const Geometry *geom, String *ptwkb);
public:
  Item_func_centroid(const POS &pos, Item *a): Item_geometry_func(pos, a) {}
  const char *func_name() const { return "st_centroid"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_convex_hull: public Item_geometry_func
{
  BG_result_buf_mgr bg_resbuf_mgr;

  template <typename Coordsys>
  bool bg_convex_hull(const Geometry *geom, String *wkb);
public:
  Item_func_convex_hull(const POS &pos, Item *a): Item_geometry_func(pos, a) {}
  const char *func_name() const { return "st_convexhull"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_envelope: public Item_geometry_func
{
public:
  Item_func_envelope(const POS &pos, Item *a): Item_geometry_func(pos, a) {}
  const char *func_name() const { return "st_envelope"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_make_envelope: public Item_geometry_func
{
public:
  Item_func_make_envelope(const POS &pos, Item *a, Item *b)
    : Item_geometry_func(pos, a, b) {}
  const char *func_name() const { return "st_makeenvelope"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};

class Item_func_validate: public Item_geometry_func
{
  String arg_val;
public:
  Item_func_validate(const POS &pos, Item *a): Item_geometry_func(pos, a) {}
  const char *func_name() const { return "st_validate"; }
  String *val_str(String *);
};

class Item_func_simplify: public Item_geometry_func
{
  BG_result_buf_mgr bg_resbuf_mgr;
  String arg_val;
  template <typename Coordsys>
  int simplify_basic(Geometry *geom, double max_dist, String *str,
                     Gis_geometry_collection *gc= NULL,
                     String *gcbuf= NULL);
public:
  Item_func_simplify(const POS &pos, Item *a, Item *b)
    : Item_geometry_func(pos, a, b) {}
  const char *func_name() const { return "st_simplify"; }
  String *val_str(String *);
};

class Item_func_point: public Item_geometry_func
{
public:
  Item_func_point(const POS &pos, Item *a, Item *b)
    : Item_geometry_func(pos, a, b)
  {}
  const char *func_name() const { return "point"; }
  String *val_str(String *);
  Field::geometry_type get_geometry_type() const;
};


/**
  This handles the @<point@> = ST_POINTFROMGEOHASH(@<string@>, @<srid@>) function.

  It returns a point containing the decoded geohash value, where X is the
  longitude in the range of [-180, 180] and Y is the latitude in the range
  of [-90, 90].

  At the moment, SRID can be any 32 bit unsigned integer.
*/
class Item_func_pointfromgeohash : public Item_geometry_func
{
private:
  /// The maximum output latitude value when decoding the geohash value.
  const double upper_latitude;

  /// The minimum output latitude value when decoding the geohash value.
  const double lower_latitude;

  /// The maximum output longitude value when decoding the geohash value.
  const double upper_longitude;

  /// The minimum output longitude value when decoding the geohash value.
  const double lower_longitude;
public:
  Item_func_pointfromgeohash(const POS &pos, Item *a, Item *b)
    : Item_geometry_func(pos, a, b),
    upper_latitude(90.0), lower_latitude(-90.0),
    upper_longitude(180.0), lower_longitude(-180.0)
  {}
  const char *func_name() const { return "st_pointfromgeohash"; }
  String *val_str(String *);
  bool fix_fields(THD *thd, Item **ref);
  Field::geometry_type get_geometry_type() const
  {
    return Field::GEOM_POINT;
  };
};


class Item_func_spatial_decomp: public Item_geometry_func
{
  enum Functype decomp_func;
public:
  Item_func_spatial_decomp(const POS &pos, Item *a, Item_func::Functype ft) :
    Item_geometry_func(pos, a)
  { decomp_func = ft; }
  const char *func_name() const
  {
    switch (decomp_func)
    {
      case SP_STARTPOINT:
        return "st_startpoint";
      case SP_ENDPOINT:
        return "st_endpoint";
      case SP_EXTERIORRING:
        return "st_exteriorring";
      default:
	DBUG_ASSERT(0);  // Should never happened
        return "spatial_decomp_unknown";
    }
  }
  String *val_str(String *);
};

class Item_func_spatial_decomp_n: public Item_geometry_func
{
  enum Functype decomp_func_n;
public:
  Item_func_spatial_decomp_n(const POS &pos, Item *a, Item *b, Item_func::Functype ft):
    Item_geometry_func(pos, a, b)
  { decomp_func_n = ft; }
  const char *func_name() const
  {
    switch (decomp_func_n)
    {
      case SP_POINTN:
        return "st_pointn";
      case SP_GEOMETRYN:
        return "st_geometryn";
      case SP_INTERIORRINGN:
        return "st_interiorringn";
      default:
	DBUG_ASSERT(0);  // Should never happened
        return "spatial_decomp_n_unknown";
    }
  }
  String *val_str(String *);
};

class Item_func_spatial_collection: public Item_geometry_func
{
  String tmp_value;
  enum Geometry::wkbType coll_type;
  enum Geometry::wkbType item_type;
public:
  Item_func_spatial_collection(const POS &pos,
     PT_item_list *list, enum Geometry::wkbType ct, enum Geometry::wkbType it):
  Item_geometry_func(pos, list)
  {
    coll_type=ct;
    item_type=it;
  }
  String *val_str(String *);
  virtual bool resolve_type(THD *thd)
  {
    if (Item_geometry_func::resolve_type(thd))
      return true;
    for (unsigned int i= 0; i < arg_count; ++i)
    {
      if (args[i]->fixed && args[i]->field_type() != MYSQL_TYPE_GEOMETRY)
      {
        String str;
        args[i]->print(&str, QT_NO_DATA_EXPANSION);
        str.append('\0');
        my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "non geometric",
                 str.ptr());
        return true;
      }
    }
    return false;
  }

  const char *func_name() const;
};


/*
  Spatial relations
*/

class Item_func_spatial_mbr_rel: public Item_bool_func2
{
  enum Functype spatial_rel;
public:
  Item_func_spatial_mbr_rel(Item *a, Item *b, enum Functype sp_rel) :
    Item_bool_func2(a, b) { spatial_rel = sp_rel; }
  Item_func_spatial_mbr_rel(const POS &pos, Item *a, Item *b,
                            enum Functype sp_rel)
  : Item_bool_func2(pos, a, b) { spatial_rel = sp_rel; }
  longlong val_int();
  enum Functype functype() const
  {
    return spatial_rel;
  }
  enum Functype rev_functype() const
  {
    switch (spatial_rel)
    {
      case SP_CONTAINS_FUNC:
        return SP_WITHIN_FUNC;
      case SP_WITHIN_FUNC:
        return SP_CONTAINS_FUNC;
      default:
        return spatial_rel;
    }
  }

  const char *func_name() const;
  virtual inline void print(String *str, enum_query_type query_type)
  {
    Item_func::print(str, query_type);
  }
  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
  bool is_null() { (void) val_int(); return null_value; }
};


class Item_func_spatial_rel: public Item_bool_func2
{
  enum Functype spatial_rel;
  String tmp_value1,tmp_value2;
public:
  Item_func_spatial_rel(const POS &pos, Item *a,Item *b, enum Functype sp_rel);
  virtual ~Item_func_spatial_rel();
  longlong val_int();
  enum Functype functype() const
  {
    return spatial_rel;
  }
  enum Functype rev_functype() const
  {
    switch (spatial_rel)
    {
      case SP_CONTAINS_FUNC:
        return SP_WITHIN_FUNC;
      case SP_WITHIN_FUNC:
        return SP_CONTAINS_FUNC;
      default:
        return spatial_rel;
    }
  }

  const char *func_name() const;
  virtual inline void print(String *str, enum_query_type query_type)
  {
    Item_func::print(str, query_type);
  }

  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
  bool is_null() { (void) val_int(); return null_value; }

  template<typename CoordinateSystemType>
  static int bg_geo_relation_check(Geometry *g1, Geometry *g2,
                                   Functype relchk_type, my_bool *);

protected:

  template<typename Geom_types>
  friend class BG_wrap;

  template<typename Geotypes>
  static int within_check(Geometry *g1, Geometry *g2,
                          my_bool *pnull_value);
  template<typename Geotypes>
  static int equals_check(Geometry *g1, Geometry *g2,
                          my_bool *pnull_value);
  template<typename Geotypes>
  static int disjoint_check(Geometry *g1, Geometry *g2,
                            my_bool *pnull_value);
  template<typename Geotypes>
  static int intersects_check(Geometry *g1, Geometry *g2,
                              my_bool *pnull_value);
  template<typename Geotypes>
  static int overlaps_check(Geometry *g1, Geometry *g2,
                            my_bool *pnull_value);
  template<typename Geotypes>
  static int touches_check(Geometry *g1, Geometry *g2,
                           my_bool *pnull_value);
  template<typename Geotypes>
  static int crosses_check(Geometry *g1, Geometry *g2,
                           my_bool *pnull_value);

  template<typename Coordsys>
  int multipoint_within_geometry_collection(Gis_multi_point *mpts,
                                            const typename
                                            BG_geometry_collection::
                                            Geometry_list *gv2,
                                            const void *prtree);

  template<typename Coordsys>
  int geocol_relation_check(Geometry *g1, Geometry *g2);
  template<typename Coordsys>
  int geocol_relcheck_intersect_disjoint(const typename BG_geometry_collection::
                                         Geometry_list *gv1,
                                         const typename BG_geometry_collection::
                                         Geometry_list *gv2);
  template<typename Coordsys>
  int geocol_relcheck_within(const typename BG_geometry_collection::
                             Geometry_list *gv1,
                             const typename BG_geometry_collection::
                             Geometry_list *gv2);
  template<typename Coordsys>
  int geocol_equals_check(const typename BG_geometry_collection::
                          Geometry_list *gv1,
                          const typename BG_geometry_collection::
                          Geometry_list *gv2);
};


/*
  Spatial operations
*/

class Item_func_spatial_operation: public Item_geometry_func
{
protected:
  // It will call the protected member functions in this class,
  // no data member accessed directly.
  template<typename Geotypes>
  friend class BG_setop_wrapper;

  // Calls bg_geo_set_op.
  friend class BG_geometry_collection;

  template<typename Coordsys>
  Geometry *bg_geo_set_op(Geometry *g1, Geometry *g2, String *result);

  template<typename Coordsys>
  Geometry *combine_sub_results(Geometry *g1, Geometry *g2, String *result);
  Geometry *simplify_multilinestring(Gis_multi_line_string *mls,
                                     String *result);

  template<typename Coordsys>
  Geometry *geometry_collection_set_operation(Geometry *g1, Geometry *g2,
                                              String *result);

  Geometry *empty_result(String *str, uint32 srid);

  String tmp_value1,tmp_value2;
  BG_result_buf_mgr bg_resbuf_mgr;

  bool assign_result(Geometry *geo, String *result);

  template <typename Geotypes>
  Geometry *intersection_operation(Geometry *g1, Geometry *g2, String *result);
  template <typename Geotypes>
  Geometry *union_operation(Geometry *g1, Geometry *g2, String *result);
  template <typename Geotypes>
  Geometry *difference_operation(Geometry *g1, Geometry *g2, String *result);
  template <typename Geotypes>
  Geometry *symdifference_operation(Geometry *g1, Geometry *g2, String *result);
  template<typename Coordsys>
  Geometry *geocol_symdifference(const BG_geometry_collection &bggc1,
                                 const BG_geometry_collection &bggc2,
                                 String *result);
  template<typename Coordsys>
  Geometry *geocol_difference(const BG_geometry_collection &bggc1,
                              const BG_geometry_collection &bggc2,
                              String *result);
  template<typename Coordsys>
  Geometry *geocol_intersection(const BG_geometry_collection &bggc1,
                                const BG_geometry_collection &bggc2,
                                String *result);
  template<typename Coordsys>
  Geometry *geocol_union(const BG_geometry_collection &bggc1,
                         const BG_geometry_collection &bggc2,
                         String *result);
public:
  enum op_type
  {
    op_shape= 0,
    op_not= 0x80000000,
    op_union= 0x10000000,
    op_intersection= 0x20000000,
    op_symdifference= 0x30000000,
    op_difference= 0x40000000,
    op_backdifference= 0x50000000,
    op_any= 0x70000000
  };

  Item_func_spatial_operation(const POS &pos, Item *a, Item *b,
                              Item_func_spatial_operation::op_type sp_op) :
    Item_geometry_func(pos, a, b), spatial_op(sp_op)
  {
  }
  virtual ~Item_func_spatial_operation();
  String *val_str(String *);
  const char *func_name() const;
  virtual inline void print(String *str, enum_query_type query_type)
  {
    Item_func::print(str, query_type);
  }
private:
  op_type spatial_op;
  String m_result_buffer;
};


class Item_func_buffer: public Item_geometry_func
{
public:
  /*
    There are five types of buffer strategies, this is an enumeration of them.
   */
  enum enum_buffer_strategy_types
  {
    invalid_strategy_type= 0,
    end_strategy,
    join_strategy,
    point_strategy,
    // The two below are not parameterized.
    distance_strategy,
    side_strategy
  };

  /*
    For each type of strategy listed above, there are several options/values
    for it, this is an enumeration of all such options/values for all types of
    strategies.
   */
  enum enum_buffer_strategies
  {
    invalid_strategy= 0,
    end_round,
    end_flat,
    join_round,
    join_miter,
    point_circle,
    point_square,
    max_strategy= point_square

    // Distance and side strategies are fixed, so no need to implement
    // parameterization for them.
  };

  /*
    A piece of strategy setting. User can specify 0 to 3 different strategy
    settings in any order to ST_Buffer(), which must be of different
    strategy types. Default strategies are used if not explicitly specified.
   */
  struct Strategy_setting
  {
    enum_buffer_strategies strategy;
    // This field is only effective for end_round, join_round, join_mit,
    // and point_circle.
    double value;
  };

private:
  BG_result_buf_mgr bg_resbuf_mgr;
  int num_strats;
  String *strategies[side_strategy + 1];
  /*
    end_xxx stored in settings[end_strategy];
    join_xxx stored in settings[join_strategy];
    point_xxx stored in settings[point_strategy].
  */
  Strategy_setting settings[side_strategy + 1];
  String tmp_value;                             // Stores current buffer result.
  String m_tmp_geombuf;
  void set_strategies();
public:
  Item_func_buffer(const POS &pos, PT_item_list *ilist);
  const char *func_name() const { return "st_buffer"; }
  String *val_str(String *);
};


class Item_func_buffer_strategy: public Item_str_func
{
private:
  friend class Item_func_buffer;
  String tmp_value;
  char tmp_buffer[16];                          // The buffer for tmp_value.
public:
  Item_func_buffer_strategy(const POS &pos, PT_item_list *ilist);
  const char *func_name() const { return "st_buffer_strategy"; }
  String *val_str(String *);
  virtual bool resolve_type(THD *thd);
};


class Item_func_isempty: public Item_bool_func
{
public:
  Item_func_isempty(const POS &pos, Item *a): Item_bool_func(pos, a) {}
  longlong val_int();
  optimize_type select_optimize() const { return OPTIMIZE_NONE; }
  const char *func_name() const { return "st_isempty"; }
  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
};

class Item_func_issimple: public Item_bool_func
{
  String tmp;
public:
  Item_func_issimple(const POS &pos, Item *a): Item_bool_func(pos, a) {}
  longlong val_int();
  bool issimple(Geometry *g);
  optimize_type select_optimize() const { return OPTIMIZE_NONE; }
  const char *func_name() const { return "st_issimple"; }
  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
};

class Item_func_isclosed: public Item_bool_func
{
public:
  Item_func_isclosed(const POS &pos, Item *a): Item_bool_func(pos, a) {}
  longlong val_int();
  optimize_type select_optimize() const { return OPTIMIZE_NONE; }
  const char *func_name() const { return "st_isclosed"; }
  virtual bool resolve_type(THD *thd)
  {
    maybe_null= true;
    return false;
  }
};

class Item_func_isvalid: public Item_bool_func
{
public:
  Item_func_isvalid(const POS &pos, Item *a): Item_bool_func(pos, a) {}
  longlong val_int();
  optimize_type select_optimize() const { return OPTIMIZE_NONE; }
  const char *func_name() const { return "st_isvalid"; }
};

class Item_func_dimension: public Item_int_func
{
  String value;
public:
  Item_func_dimension(const POS &pos, Item *a): Item_int_func(pos, a) {}
  longlong val_int();
  const char *func_name() const { return "st_dimension"; }
  virtual bool resolve_type(THD *thd)
  {
    max_length= 10;
    maybe_null= true;
    return false;
  }
};

/**
  This class updates the x coordinate of geometry class POINT.
  The class handles the SQL function @<geometry@>= ST_X(@<point@>, @<double@>).
*/
class Item_func_set_x : public Item_geometry_func
{
public:
  Item_func_set_x(const POS &pos, Item *a, Item *b) :
    Item_geometry_func(pos, a, b) {}
  const char *func_name() const { return "st_x"; }
  String *val_str(String *);
};

/**
  This class updates the y coordinate of geometry class POINT.
  The class handles the SQL function @<geometry@>= ST_Y(@<point@>, @<double@>).
*/
class Item_func_set_y : public Item_geometry_func
{
public:
  Item_func_set_y(const POS &pos, Item *a, Item *b) :
    Item_geometry_func(pos, a, b) {}
  const char *func_name() const { return "st_y"; }
  String *val_str(String *);
};


class Item_func_get_x: public Item_real_func
{
  String value;
public:
  Item_func_get_x(const POS &pos, Item *a): Item_real_func(pos, a) {}
  double val_real();
  const char *func_name() const { return "st_x"; }
  virtual bool resolve_type(THD *thd)
  {
    if (Item_real_func::resolve_type(thd))
      return true;
    maybe_null= true;
    return false;
  }
};


class Item_func_get_y: public Item_real_func
{
  String value;
public:
  Item_func_get_y(const POS &pos, Item *a): Item_real_func(pos, a) {}
  double val_real();
  const char *func_name() const { return "st_y"; }
  virtual bool resolve_type(THD *thd)
  {
    if (Item_real_func::resolve_type(thd))
      return true;
    maybe_null= true;
    return false;
  }
};


class Item_func_numgeometries: public Item_int_func
{
  String value;
public:
  Item_func_numgeometries(const POS &pos, Item *a): Item_int_func(pos, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numgeometries"; }
  virtual bool resolve_type(THD *thd)
  {
    max_length= 10;
    maybe_null= true;
    return false;
  }
};


class Item_func_numinteriorring: public Item_int_func
{
  String value;
public:
  Item_func_numinteriorring(const POS &pos, Item *a): Item_int_func(pos, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numinteriorrings"; }
  virtual bool resolve_type(THD *thd)
  {
    max_length= 10;
    maybe_null= true;
    return false;
  }
};


class Item_func_numpoints: public Item_int_func
{
  String value;
public:
  Item_func_numpoints(const POS &pos, Item *a): Item_int_func(pos, a) {}
  longlong val_int();
  const char *func_name() const { return "st_numpoints"; }
  virtual bool resolve_type(THD *thd)
  {
    max_length= 10;
    maybe_null= true;
    return false;
  }
};


class Item_func_area: public Item_real_func
{
  String value;

  template <typename Coordsys>
  double bg_area(const Geometry *geom);
public:
  Item_func_area(const POS &pos, Item *a): Item_real_func(pos, a) {}
  double val_real();
  const char *func_name() const { return "st_area"; }
  virtual bool resolve_type(THD *thd)
  {
    if (Item_real_func::resolve_type(thd))
      return true;
    maybe_null= true;
    return false;
  }
};


class Item_func_glength: public Item_real_func
{
  String value;
public:
  Item_func_glength(const POS &pos, Item *a): Item_real_func(pos, a) {}
  double val_real();
  const char *func_name() const { return "st_length"; }
  virtual bool resolve_type(THD *thd)
  {
    if (Item_real_func::resolve_type(thd))
      return true;
    maybe_null= true;
    return false;
  }
};


/**
  Implements the one-parameter ST_SRID observer function. If used with a
  valid geometry, it returns the SRID of the geometry object. Ex:

  SELECT ST_SRID(geometry_column) FROM t1;

  will return the SRIDs of the geometries in geometry_column
  in table t1.
*/
class Item_func_get_srid: public Item_int_func
{
  String value;
public:
  Item_func_get_srid(const POS &pos, Item *a): Item_int_func(pos, a) {}
  longlong val_int();
  const char *func_name() const { return "st_srid"; }
  virtual bool resolve_type(THD *thd)
  {
    max_length= 10;
    maybe_null= true;
    return false;
  }
};


/**
  Updates the SRID of a geometry object without changing its content.
  This extends the ST_SRID function to two parameters: a geometry and an
  SRID. Can be used as follows:

  UPDATE t1 SET geometry_column=ST_SRID(geometry_column, 4326);

  This will update all geometries in geometry_column to have SRID 4326.
*/
class Item_func_set_srid: public Item_geometry_func
{
public:
  Item_func_set_srid(const POS &pos, Item *a, Item *b)
                     : Item_geometry_func(pos, a, b){};
  String *val_str(String *str);
  const char *func_name() const {return "st_srid"; }
};


class Item_func_distance: public Item_real_func
{
  // Default earth radius in meters.
  bool is_spherical_equatorial;
  double earth_radius;
  String tmp_value1;
  String tmp_value2;

  double geometry_collection_distance(const Geometry *g1, const Geometry *g2);

  template <typename Coordsys, typename BG_geometry>
  double distance_dispatch_second_geometry(const BG_geometry& bg1,
                                           const Geometry* g2);

  double distance_point_geometry_spherical(const Geometry *g1,
                                           const Geometry *g2);
  double distance_multipoint_geometry_spherical(const Geometry *g1,
                                                const Geometry *g2);
public:
  double bg_distance_spherical(const Geometry *g1, const Geometry *g2);
  template <typename Coordsys>
  double bg_distance(const Geometry *g1, const Geometry *g2);

  Item_func_distance(const POS &pos, PT_item_list *ilist, bool isspherical)
    : Item_real_func(pos, ilist), is_spherical_equatorial(isspherical),
      earth_radius(6370986.0)                   /* Default earth radius. */
  {
    /*
      Either operand can be an empty geometry collection, and it's meaningless
      for a distance between them.
    */
    maybe_null= true;
  }

  virtual bool resolve_type(THD *thd)
  {
    if (Item_real_func::resolve_type(thd))
      return true;
    maybe_null= true;
    return false;
  }

  double val_real();
  const char *func_name() const
  {
    return is_spherical_equatorial ? "st_distance_sphere" : "st_distance";
  }
};

#endif /*ITEM_GEOFUNC_INCLUDED*/
