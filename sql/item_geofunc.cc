/* Copyright (c) 2003, 2016, Oracle and/or its affiliates. All rights reserved.

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


/**
  @file

  @brief
  This file defines all spatial functions
*/
#include "item_geofunc.h"

#include "current_thd.h"
#include "gstream.h"      // Gis_read_stream
#include "psi_memory_key.h"
#include "sql_class.h"    // THD
#include "gis_bg_traits.h"
#include "derror.h"       // ER_THD

#include "parse_tree_helpers.h"
#include "item_geofunc_internal.h"
#include "json_dom.h"     // Json_wrapper

#include "dd/types/spatial_reference_system.h"

#include <cmath>          // isfinite
#include <stack>


static int check_geometry_valid(Geometry *geom);


/**
  Check if a Item is considered to be a SQL NULL value.

  This includes NULL in the following forms:
    @verbatim
    - Literal NULL (e.g. SELECT NULL;)
    - NULL in a user-defined variable (SET @foo = NULL;)
    - NULL in prepared statements (PREPARE stmt FROM 'SELECT ?';).
    @endverbatim

  If a SQL NULL is provided via an user-defined variable, it will be detected
  as a FUNC_ITEM with MYSQL_TYPE_MEDIUM_BLOB as the field type.

  @param item The item to check for SQL NULL.

  @return true if the input is considered as NULL, false otherwise.
*/
static bool is_item_null(Item *item)
{
  if (item->field_type() == MYSQL_TYPE_NULL || item->type() == Item::NULL_ITEM)
    return true;

  // The following will detect the usage of SQL NULL in user-defined variables.
  bool is_binary_charset= (item->collation.collation == &my_charset_bin);
  if (is_binary_charset && item->type() == Item::FUNC_ITEM &&
      item->field_type() == MYSQL_TYPE_MEDIUM_BLOB)
  {
    return true;
  }
  return false;
}


/**
  Check if an Item is considered to be a geometry type.

  We do the following checks to determine if the Item is a geometry type:
    - NULL is considered a valid geometry type.
    - If the field_type is MYSQL_TYPE_GEOMETRY, it is indeed a geometry.
    - If the type() is PARAM_ITEM, we accept it as a geometry type. This is used
      to allow for parameter markers.
    - If the collation of the Item is binary, we accept it as a geometry type.
      This is used to allow for user-defined variables.

  @param item The Item to verify as a geometry type.

  @return true if the Item is considered to be a geometry type, false otherwise.
*/
static bool is_item_geometry_type(Item *item)
{
  if (is_item_null(item))
    return true;
  else if (item->field_type() == MYSQL_TYPE_GEOMETRY)
    return true;
  else if (item->type() == Item::PARAM_ITEM)
    return true;
  else if (item->collation.collation == &my_charset_bin)
    return true;
  return false;
}


/**
  Check whether all points in the sequence container are colinear.
  @param ls a sequence of points with no duplicates.
  @return true if the points are colinear, false otherwise.
*/
template <typename Point_range>
bool is_colinear(const Point_range &ls)
{
  if (ls.size() < 3)
    return true;

  double x1, x2, x3, y1, y2, y3, X1, X2, Y1, Y2;

  for (size_t i= 0; i < ls.size() - 2; i++)
  {
    x1= ls[i].template get<0>();
    x2= ls[i + 1].template get<0>();
    x3= ls[i + 2].template get<0>();

    y1= ls[i].template get<1>();
    y2= ls[i + 1].template get<1>();
    y3= ls[i + 2].template get<1>();

    X1= x2 - x1;
    X2= x3 - x2;
    Y1= y2 - y1;
    Y2= y3 - y2;

    if (X1 * Y2 - X2 * Y1 != 0)
      return false;
  }

  return true;
}


/**
  Get an SRID from an item and check that it's within bounds.

  If the item is NULL, set null_value.

  The value must be representable as a 32 bit unsigned integer. If the
  value is outside this range, an error is flagged.

  @param[in] arg Item that holds the SRID
  @param[out] srid Where to store the SRID
  @param[out] null_value Where to store the null_value
  @param[in] func_name Function name to use in error messages

  @retval true An error has occurred
  @retval false Success
*/
static bool validate_srid_arg(Item *arg, Geometry::srid_t *srid,
                              my_bool *null_value, const char *func_name)
{
  longlong arg_srid= arg->val_int();

  if ((*null_value= arg->null_value))
  {
    return false;
  }

  if (arg_srid < 0 || arg_srid > UINT_MAX32)
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "SRID", func_name);
    return true;
  }

  *srid= static_cast<Geometry::srid_t>(arg_srid);
  return false;
}


Item_geometry_func::Item_geometry_func(const POS &pos, PT_item_list *list)
  :Item_str_func(pos, list)
{}


Field *Item_geometry_func::tmp_table_field(TABLE *t_arg)
{
  Field *result;
  if ((result= new Field_geom(max_length, maybe_null, item_name.ptr(),
                              get_geometry_type())))
    result->init(t_arg);
  return result;
}


bool Item_geometry_func::resolve_type(THD *thd)
{
  collation.set(&my_charset_bin);
  decimals=0;
  max_length= 0xFFFFFFFFU;
  maybe_null= true;
  return false;
}


bool Item_func_geometry_from_text::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  DBUG_ASSERT(arg_count == 1 || arg_count == 2);
  if (arg_count == 1)
    pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return false;
}


const char *Item_func_geometry_from_text::func_name() const
{
  switch (m_functype)
  {
  case Functype::GEOMCOLLFROMTEXT:
    return "st_geomcollfromtext";
  case Functype::GEOMCOLLFROMTXT:
    return "st_geomcollfromtxt";
  case Functype::GEOMETRYCOLLECTIONFROMTEXT:
    return "st_geometrycollectionfromtext";
  case Functype::GEOMETRYFROMTEXT:
    return "st_geometryfromtext";
  case Functype::GEOMFROMTEXT:
    return "st_geomfromtext";
  case Functype::LINEFROMTEXT:
    return "st_linefromtext";
  case Functype::LINESTRINGFROMTEXT:
    return "st_linestringfromtext";
  case Functype::MLINEFROMTEXT:
    return "st_mlinefromtext";
  case Functype::MPOINTFROMTEXT:
    return "st_mpointfromtext";
  case Functype::MPOLYFROMTEXT:
    return "st_mpolyfromtext";
  case Functype::MULTILINESTRINGFROMTEXT:
    return "st_multilinestringfromtext";
  case Functype::MULTIPOINTFROMTEXT:
    return "st_multipointfromtext";
  case Functype::MULTIPOLYGONFROMTEXT:
    return "st_multipolygonfromtext";
  case Functype::POINTFROMTEXT:
    return "st_pointfromtext";
  case Functype::POLYFROMTEXT:
    return "st_polyfromtext";
  case Functype::POLYGONFROMTEXT:
    return "st_polygonfromtext";
  }

  DBUG_ASSERT(false); // Unreachable.
  return "st_geomfromtext";
}


Geometry::wkbType Item_func_geometry_from_text::allowed_wkb_type() const
{
  switch (m_functype)
  {
  case Functype::GEOMETRYFROMTEXT:
  case Functype::GEOMFROMTEXT:
    return Geometry::wkb_invalid_type;
  case Functype::POINTFROMTEXT:
    return Geometry::wkb_point;
  case Functype::LINEFROMTEXT:
  case Functype::LINESTRINGFROMTEXT:
    return Geometry::wkb_linestring;
  case Functype::POLYFROMTEXT:
  case Functype::POLYGONFROMTEXT:
    return Geometry::wkb_polygon;
  case Functype::MPOINTFROMTEXT:
  case Functype::MULTIPOINTFROMTEXT:
    return Geometry::wkb_multipoint;
  case Functype::MLINEFROMTEXT:
  case Functype::MULTILINESTRINGFROMTEXT:
    return Geometry::wkb_multilinestring;
  case Functype::MPOLYFROMTEXT:
  case Functype::MULTIPOLYGONFROMTEXT:
    return Geometry::wkb_multipolygon;
  case Functype::GEOMCOLLFROMTEXT:
  case Functype::GEOMCOLLFROMTXT:
  case Functype::GEOMETRYCOLLECTIONFROMTEXT:
    return Geometry::wkb_geometrycollection;
  }

  DBUG_ASSERT(false); // Unreachable.
  return Geometry::wkb_invalid_type;
}


bool Item_func_geometry_from_text::is_allowed_wkb_type(Geometry::wkbType type)
  const
{
  // Allowed if
  // 1. type is the allowed type, or
  // 2. the allowed type is Geometry, or
  // 3. the allowed type is GeometryCollection and type is a subtype
  //    of GeometryCollection
  if (type == allowed_wkb_type() ||                              // 1
      allowed_wkb_type() == Geometry::wkb_invalid_type ||        // 2
      (allowed_wkb_type() == Geometry::wkb_geometrycollection && // 3
       (type == Geometry::wkb_multipoint ||
        type == Geometry::wkb_multilinestring ||
        type == Geometry::wkb_multipolygon)))
  {
    return true;
  }

  return false;
}


/**
  Parses a WKT string to produce a geometry encoded with an SRID prepending
  its WKB bytes, namely a byte string of GEOMETRY format.
  @param str buffer to hold result, may not be filled.
  @return the buffer that hold the GEOMETRY byte string result, may or may
  not be the same as 'str' parameter.
 */
String *Item_func_geometry_from_text::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  Geometry_buffer buffer;
  String arg_val;
  String *wkt= args[0]->val_str_ascii(&arg_val);

  if ((null_value= (!wkt || args[0]->null_value)))
    return nullptr;

  Gis_read_stream trs(wkt->charset(), wkt->ptr(), wkt->length());
  Geometry::srid_t srid= 0;

  if (arg_count == 2)
  {
    if (validate_srid_arg(args[1], &srid, &null_value, func_name()))
      return error_str();
    if (null_value)
    {
      DBUG_ASSERT(maybe_null);
      return nullptr;
    }
  }

  str->set_charset(&my_charset_bin);
  if (str->reserve(GEOM_HEADER_SIZE, 512))
    return error_str();
  str->length(0);
  str->q_append(srid);
  Geometry *g= Geometry::create_from_wkt(&buffer, &trs, str, 0);
  if (g == nullptr)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }
  if (!is_allowed_wkb_type(g->get_type()))
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "WKT",
             g->get_class_info()->m_name.str, func_name());
    return error_str();
  }

  return str;
}


bool Item_func_geometry_from_wkb::itemize(Parse_context *pc, Item **res)
{
  if (skip_itemize(res))
    return false;
  if (super::itemize(pc, res))
    return true;
  DBUG_ASSERT(arg_count == 1 || arg_count == 2);
  if (arg_count == 1)
    pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return false;
}


const char *Item_func_geometry_from_wkb::func_name() const
{
  switch (m_functype)
  {
  case Functype::GEOMCOLLFROMWKB:
    return "st_geomcollfromwkb";
  case Functype::GEOMETRYCOLLECTIONFROMWKB:
    return "st_geometrycollectionfromwkb";
  case Functype::GEOMETRYFROMWKB:
    return "st_geometryfromwkb";
  case Functype::GEOMFROMWKB:
    return "st_geomfromwkb";
  case Functype::LINEFROMWKB:
    return "st_linefromwkb";
  case Functype::LINESTRINGFROMWKB:
    return "st_linestringfromwkb";
  case Functype::MLINEFROMWKB:
    return "st_mlinefromwkb";
  case Functype::MPOINTFROMWKB:
    return "st_mpointfromwkb";
  case Functype::MPOLYFROMWKB:
    return "st_mpolyfromwkb";
  case Functype::MULTILINESTRINGFROMWKB:
    return "st_multilinestringfromwkb";
  case Functype::MULTIPOINTFROMWKB:
    return "st_multipointfromwkb";
  case Functype::MULTIPOLYGONFROMWKB:
    return "st_multipolygonfromwkb";
  case Functype::POINTFROMWKB:
    return "st_pointfromwkb";
  case Functype::POLYFROMWKB:
    return "st_polyfromwkb";
  case Functype::POLYGONFROMWKB:
    return "st_polygonfromwkb";
  }

  DBUG_ASSERT(false); // Unreachable.
  return "st_geomfromwkb";
}


Geometry::wkbType Item_func_geometry_from_wkb::allowed_wkb_type() const
{
  switch (m_functype)
  {
  case Functype::GEOMETRYFROMWKB:
  case Functype::GEOMFROMWKB:
    return Geometry::wkb_invalid_type;
  case Functype::POINTFROMWKB:
    return Geometry::wkb_point;
  case Functype::LINEFROMWKB:
  case Functype::LINESTRINGFROMWKB:
    return Geometry::wkb_linestring;
  case Functype::POLYFROMWKB:
  case Functype::POLYGONFROMWKB:
    return Geometry::wkb_polygon;
  case Functype::MPOINTFROMWKB:
  case Functype::MULTIPOINTFROMWKB:
    return Geometry::wkb_multipoint;
  case Functype::MLINEFROMWKB:
  case Functype::MULTILINESTRINGFROMWKB:
    return Geometry::wkb_multilinestring;
  case Functype::MPOLYFROMWKB:
  case Functype::MULTIPOLYGONFROMWKB:
    return Geometry::wkb_multipolygon;
  case Functype::GEOMCOLLFROMWKB:
  case Functype::GEOMETRYCOLLECTIONFROMWKB:
    return Geometry::wkb_geometrycollection;
  }

  DBUG_ASSERT(false); // Unreachable.
  return Geometry::wkb_invalid_type;
}


bool
Item_func_geometry_from_wkb::is_allowed_wkb_type(Geometry::wkbType type) const
{
  // Allowed if
  // 1. type is the allowed type, or
  // 2. the allowed type is Geometry, or
  // 3. the allowed type is GeometryCollection and type is a subtype
  //    of GeometryCollection
  if (type == allowed_wkb_type() ||                              // 1
      allowed_wkb_type() == Geometry::wkb_invalid_type ||        // 2
      (allowed_wkb_type() == Geometry::wkb_geometrycollection && // 3
       (type == Geometry::wkb_multipoint ||
        type == Geometry::wkb_multilinestring ||
        type == Geometry::wkb_multipolygon)))
  {
    return true;
  }

  return false;
}


/**
  Parses a WKT string to produce a geometry encoded with an SRID prepending
  its WKB bytes, namely a byte string of GEOMETRY format.
  @param str buffer to hold result, may not be filled.
  @return the buffer that hold the GEOMETRY byte string result, may or may
  not be the same as 'str' parameter.
 */
String *Item_func_geometry_from_wkb::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  Geometry::srid_t srid= 0;

  if (arg_count == 2)
  {
    if (validate_srid_arg(args[1], &srid, &null_value, func_name()))
      return error_str();
    if (null_value)
    {
      DBUG_ASSERT(maybe_null);
      return nullptr;
    }
  }

  String *wkb= args[0]->val_str(&tmp_value);
  if ((null_value= (!wkb || args[0]->null_value)))
  {
    DBUG_ASSERT(maybe_null);
    return nullptr;
  }

  /*
    GeometryFromWKB(wkb [,srid]) understands both WKB (without SRID) and
    Geometry (with SRID) values in the "wkb" argument.
    In case if a Geometry type value is passed, we assume that the value
    is well-formed and can directly return it without going through
    Geometry::create_from_wkb(), and consequently such WKB data must be
    MySQL standard (little) endian. Note that users can pass via client
    any WKB/Geometry byte string, including those of big endianess.
  */
  if (args[0]->field_type() == MYSQL_TYPE_GEOMETRY)
  {
    Geometry_buffer buff;
    Geometry *g= Geometry::construct(&buff, wkb->ptr(), wkb->length());
    if (g == nullptr)
    {
      my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
      return error_str();
    }
    if (!is_allowed_wkb_type(g->get_type()))
    {
      my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "WKB",
               g->get_class_info()->m_name.str, func_name());
      return error_str();
    }

    /*
      Check if SRID embedded into the Geometry value differs
      from the SRID value passed in the second argument.
    */
    if (srid == uint4korr(wkb->ptr()))
      return wkb; // Do not differ

    /*
      Replace SRID to the one passed in the second argument.
      Note, we cannot replace SRID directly in wkb->ptr(),
      because wkb can point to some value that we should not touch,
      e.g. to a SP variable value. So we need to copy to "str".
    */
    if (str->copy(*wkb))
      return error_str();
    str->write_at_position(0, srid);
    return str;
  }

  str->set_charset(&my_charset_bin);
  if (str->reserve(GEOM_HEADER_SIZE, 512))
  {
    return error_str();
  }
  str->length(0);
  str->q_append(srid);
  Geometry_buffer buffer;
  Geometry *g= Geometry::create_from_wkb(&buffer, wkb->ptr(), wkb->length(),
                                         str, false /* Don't init stream. */);
  if (g == nullptr)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }
  if (!is_allowed_wkb_type(g->get_type()))
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "WKB",
             g->get_class_info()->m_name.str, func_name());
    return error_str();
  }

  return str;
}


/**
  Definition of various string constants used for writing and reading
  GeoJSON data.
*/
const char *Item_func_geomfromgeojson::TYPE_MEMBER= "type";
const char *Item_func_geomfromgeojson::CRS_MEMBER= "crs";
const char *Item_func_geomfromgeojson::GEOMETRY_MEMBER= "geometry";
const char *Item_func_geomfromgeojson::PROPERTIES_MEMBER= "properties";
const char *Item_func_geomfromgeojson::FEATURES_MEMBER= "features";
const char *Item_func_geomfromgeojson::GEOMETRIES_MEMBER= "geometries";
const char *Item_func_geomfromgeojson::COORDINATES_MEMBER= "coordinates";
const char *Item_func_geomfromgeojson::CRS_NAME_MEMBER= "name";
const char *Item_func_geomfromgeojson::NAMED_CRS= "name";
const char *Item_func_geomfromgeojson::SHORT_EPSG_PREFIX= "EPSG:";
const char *Item_func_geomfromgeojson::POINT_TYPE= "Point";
const char *Item_func_geomfromgeojson::MULTIPOINT_TYPE= "MultiPoint";
const char *Item_func_geomfromgeojson::LINESTRING_TYPE= "LineString";
const char *Item_func_geomfromgeojson::MULTILINESTRING_TYPE= "MultiLineString";
const char *Item_func_geomfromgeojson::POLYGON_TYPE= "Polygon";
const char *Item_func_geomfromgeojson::MULTIPOLYGON_TYPE= "MultiPolygon";
const char *Item_func_geomfromgeojson::FEATURE_TYPE= "Feature";
const char *Item_func_geomfromgeojson::
FEATURECOLLECTION_TYPE= "FeatureCollection";
const char *Item_func_geomfromgeojson::
LONG_EPSG_PREFIX= "urn:ogc:def:crs:EPSG::";
const char *Item_func_geomfromgeojson::
CRS84_URN= "urn:ogc:def:crs:OGC:1.3:CRS84";
const char *Item_func_geomfromgeojson::
GEOMETRYCOLLECTION_TYPE= "GeometryCollection";


/**
  @<geometry@> = ST_GEOMFROMGEOJSON(@<string@>[, @<options@>[, @<srid@>]])

  Takes a GeoJSON input string and outputs a GEOMETRY.
  This function supports both single GeoJSON objects and geometry collections.

  In addition, feature objects and feature collections are supported (feature
  collections are translated into GEOMETRYCOLLECTION).

  It follows the standard described at http://geojson.org/geojson-spec.html
  (revision 1.0).
*/
String *Item_func_geomfromgeojson::val_str(String *buf)
{
  if (arg_count > 1)
  {
    // Check and parse the OPTIONS parameter.
    longlong dimension_argument= args[1]->val_int();
    if ((null_value= args[1]->null_value))
      return NULL;

    if (dimension_argument == 1)
    {
      m_handle_coordinate_dimension= Item_func_geomfromgeojson::reject_document;
    }
    else if (dimension_argument == 2)
    {
      m_handle_coordinate_dimension=
        Item_func_geomfromgeojson::strip_now_accept_future;
    }
    else if (dimension_argument == 3)
    {
      m_handle_coordinate_dimension=
        Item_func_geomfromgeojson::strip_now_reject_future;
    }
    else if (dimension_argument == 4)
    {
      m_handle_coordinate_dimension=
        Item_func_geomfromgeojson::strip_now_strip_future;
    }
    else
    {
      char option_string[MAX_BIGINT_WIDTH + 1];
      if (args[1]->unsigned_flag)
        ullstr(dimension_argument, option_string);
      else
        llstr(dimension_argument, option_string);

      my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "option", option_string,
               func_name());
      return error_str();
    }
  }

  if (arg_count > 2)
  {
    /*
      Check and parse the SRID parameter. If this is set to a valid value,
      any CRS member in the GeoJSON document will be ignored.
    */
    if (validate_srid_arg(args[2], &m_user_srid, &null_value, func_name()))
      return error_str();
    if (null_value)
    {
      DBUG_ASSERT(maybe_null);
      return nullptr;
    }

    m_user_provided_srid= true;
  }

  Json_wrapper wr;
  if (get_json_wrapper(args, 0, buf, func_name(), &wr, true))
    return error_str();

  /*
    We will handle JSON NULL the same way as we handle SQL NULL. The reason
    behind this is that we want the following SQL to return SQL NULL:

      SELECT ST_GeomFromGeoJSON(
        JSON_EXTRACT(
          '{ "type": "Feature",
             "geometry": null,
             "properties": { "name": "Foo" }
           }',
        '$.geometry')
      );

    The function JSON_EXTRACT will return a JSON NULL, so if we don't handle
    JSON NULL as SQL NULL the above SQL will raise an error since we would
    expect a SQL NULL or a JSON object.
  */
  null_value= (args[0]->null_value || wr.type() == enum_json_type::J_NULL);
  if (null_value)
  {
    DBUG_ASSERT(maybe_null);
    return nullptr;
  }

  if (wr.type() != enum_json_type::J_OBJECT)
  {
    my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
    return error_str();
  }
  const Json_object *root_obj=
    down_cast<const Json_object*>(wr.to_dom(current_thd));

  /*
    Set the default SRID to 4326. This will be overwritten if a valid CRS is
    found in the GeoJSON input, or if the user has specified a SRID as an
    argument.

    It would probably be smart to allocate a percentage of the length of the
    input string (something like buf->realloc(json_string->length() * 0.2)).
    This would save a lot of reallocations and boost performance, especially for
    large inputs. But it is difficult to predict how much of the json input that
    will be parsed into output data.
  */
  if (buf->reserve(GEOM_HEADER_SIZE, 512))
  {
    my_error(ER_OUTOFMEMORY, GEOM_HEADER_SIZE);
    return error_str();
  }
  buf->set_charset(&my_charset_bin);
  buf->length(0);
  buf->q_append(static_cast<uint32>(4326));

  /*
    The rollback variable is used for detecting/accepting NULL objects inside
    collections (a feature with NULL geometry is allowed, and thus we can have
    a geometry collection with a NULL geometry translated into following WKT:
    GEOMETRYCOLLECTION EMPTY).

    parse_object() does a recursive parsing of the GeoJSON document.
  */
  String collection_buffer;
  bool rollback= false;
  Geometry *result_geometry= NULL;

  m_srid_found_in_document = -1;
  if (parse_object(root_obj, &rollback, &collection_buffer, false,
                   &result_geometry))
  {
    // Do a delete here, to be sure that we have no memory leaks.
    delete result_geometry;
    result_geometry= NULL;

    if (rollback)
    {
      DBUG_ASSERT(maybe_null);
      null_value= true;
      return nullptr;
    }
    return error_str();
  }

  // Set the correct SRID for the geometry data.
  if (m_user_provided_srid)
    buf->write_at_position(0, m_user_srid);
  else if (m_srid_found_in_document > -1)
    buf->write_at_position(0, static_cast<uint32>(m_srid_found_in_document));

  bool return_result= result_geometry->as_wkb(buf, false);

  delete result_geometry;
  result_geometry= NULL;

  if (return_result)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }
  return buf;
}


/**
  Case insensitive lookup of a member in a JSON object.

  This is needed since the get()-method of the JSON object is case
  sensitive.

  @param object The object to look for the member in.
  @param member_name Name of the member to look after

  @return The member if one was found, NULL otherwise.
*/
const Json_dom *Item_func_geomfromgeojson::
my_find_member_ncase(const Json_object *object, const char *member_name)
{
  Json_object::const_iterator itr;
  for (itr= object->begin(); itr != object->end(); ++itr)
  {
    if (native_strcasecmp(member_name, itr->first.c_str()) == 0)
      return itr->second;
  }

  return NULL;
}


/**
  Takes a JSON object as input, and parses the data to a Geometry object.

  The call stack will be no larger than the maximum depth of the GeoJSON
  document, which is more or less equivalent to the number of nested
  collections in the document.

  @param object A JSON object object to parse.
  @param rollback Pointer to a boolean indicating if parsed data should
         be reverted/rolled back.
  @param buffer A string buffer to be used by GeometryCollection
  @param is_parent_featurecollection Indicating if the current geometry is a
         child of a FeatureCollection.
  @param[out] geometry A pointer to the parsed geometry.

  @return true if the parsing failed, false otherwise. Note that if rollback is
          set to true and true is returned, the parsing succeeded, but no
          Geometry data could be parsed.
*/
bool Item_func_geomfromgeojson::
parse_object(const Json_object *object, bool *rollback, String *buffer,
             bool is_parent_featurecollection, Geometry **geometry)
{
  /*
    A GeoJSON object MUST have a type member, which MUST
    be of string type.
  */
  const Json_dom *type_member= my_find_member_ncase(object, TYPE_MEMBER);
  if (!is_member_valid(type_member, TYPE_MEMBER, enum_json_type::J_STRING,
                       false, NULL))
  {
    return true;
  }

  /*
    Check if this object has a CRS member.
    We allow the CRS member to be JSON NULL.
  */
  const Json_dom *crs_member= my_find_member_ncase(object, CRS_MEMBER);
  if (crs_member != NULL)
  {
    if (crs_member->json_type() == enum_json_type::J_OBJECT)
    {
      const Json_object *crs_obj= down_cast<const Json_object *>(crs_member);
      if (parse_crs_object(crs_obj))
        return true;
    }
    else if (crs_member->json_type() != enum_json_type::J_NULL)
    {
      my_error(ER_INVALID_GEOJSON_WRONG_TYPE, MYF(0), func_name(),
               CRS_MEMBER, "object");
      return true;
    }
  }

  // Handle feature objects and feature collection objects.
  const Json_string *type_member_str=
    down_cast<const Json_string*>(type_member);
  if (strcmp(type_member_str->value().c_str(), FEATURE_TYPE) == 0)
  {
    /*
      Check if this feature object has the required "geometry" and "properties"
      member. Note that we do not use the member "properties" for anything else
      than checking for valid GeoJSON document.
    */
    bool dummy;
    const Json_dom *geometry_member= my_find_member_ncase(object,
                                                          GEOMETRY_MEMBER);
    const Json_dom *properties_member= my_find_member_ncase(object,
                                                            PROPERTIES_MEMBER);
    if (!is_member_valid(geometry_member, GEOMETRY_MEMBER,
                         enum_json_type::J_OBJECT, true, rollback) ||
        !is_member_valid(properties_member, PROPERTIES_MEMBER,
                         enum_json_type::J_OBJECT, true, &dummy) || *rollback)
    {
      return true;
    }

    const Json_object *geometry_member_obj=
      down_cast<const Json_object*>(geometry_member);

    return parse_object(geometry_member_obj, rollback, buffer, false, geometry);
  }
  else if (strcmp(type_member_str->value().c_str(), FEATURECOLLECTION_TYPE) == 0)
  {
    // FeatureCollections cannot be nested according to GeoJSON spec.
    if (is_parent_featurecollection)
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }

    // We will handle a FeatureCollection as a GeometryCollection.
    const Json_dom *features= my_find_member_ncase(object, FEATURES_MEMBER);
    if (!is_member_valid(features, FEATURES_MEMBER, enum_json_type::J_ARRAY,
                         false, NULL))
    {
      return true;
    }

    const Json_array *features_array= down_cast<const Json_array*>(features);
    return parse_object_array(features_array, Geometry::wkb_geometrycollection,
                              rollback, buffer, true, geometry);
  }
  else
  {
    Geometry::wkbType wkbtype= get_wkbtype(type_member_str->value().c_str());
    if (wkbtype == Geometry::wkb_invalid_type)
    {
      // An invalid GeoJSON type was found.
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }
    else
    {
      /*
        All objects except GeometryCollection MUST have a member "coordinates"
        of type array. GeometryCollection MUST have a member "geometries" of
        type array.
      */
      const char *member_name;
      if (wkbtype == Geometry::wkb_geometrycollection)
        member_name= GEOMETRIES_MEMBER;
      else
        member_name= COORDINATES_MEMBER;

      const Json_dom *array_member= my_find_member_ncase(object, member_name);
      if (!is_member_valid(array_member, member_name, enum_json_type::J_ARRAY,
                           false, NULL))
      {
        return true;
      }

      const Json_array *array_member_array=
        down_cast<const Json_array*>(array_member);
      return parse_object_array(array_member_array, wkbtype, rollback, buffer,
                                false, geometry);
    }
  }

  // Defensive code. This should never be reached.
  /* purecov: begin inspected */
  DBUG_ASSERT(false);
  return true;
  /* purecov: end inspected */
}


/**
  Parse an array of coordinates to a Gis_point.

  Parses an array of coordinates to a Gis_point. This function must handle
  according to the handle_dimension parameter on how non 2D objects should be
  handled.

  According to the specification, a position array must have at least two
  elements, but there is no upper limit.

  @param coordinates A JSON array of coordinates.
  @param[out] point A pointer to the parsed Gis_point.

  @return true if the parsing failed, false otherwise.
*/
bool Item_func_geomfromgeojson::
get_positions(const Json_array *coordinates, Gis_point *point)
{
  /*
    According to GeoJSON specification, a position array must have at least
    two positions.
  */
  if (coordinates->size() < 2)
  {
    my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
    return true;
  }

  switch (m_handle_coordinate_dimension)
  {
  case Item_func_geomfromgeojson::reject_document:
    if (coordinates->size() > GEOM_DIM)
    {
      my_error(ER_DIMENSION_UNSUPPORTED, MYF(0), func_name(),
               coordinates->size(), GEOM_DIM);
      return true;
    }
    break;
  case Item_func_geomfromgeojson::strip_now_reject_future:
    /*
      The version in development as of writing, only supports 2 dimensions.
      When dimension count is increased beyond 2, we want the function to fail.
    */
    if (GEOM_DIM > 2 && coordinates->size() > 2)
    {
      my_error(ER_DIMENSION_UNSUPPORTED, MYF(0), func_name(),
               coordinates->size(), GEOM_DIM);
      return true;
    }
    break;
  case Item_func_geomfromgeojson::strip_now_strip_future:
  case Item_func_geomfromgeojson::strip_now_accept_future:
    if (GEOM_DIM > 2)
      DBUG_ASSERT(false);
    break;
  default:
    // Unspecified behaviour.
    DBUG_ASSERT(false);
    return true;
  }

  // Check if all array members are numbers.
  for (size_t i= 0; i < coordinates->size(); ++i)
  {
    if (!(*coordinates)[i]->is_number())
    {
      my_error(ER_INVALID_GEOJSON_WRONG_TYPE, MYF(0), func_name(),
               "array coordinate", "number");
      return true;
    }

    /*
      Even though we only need the two first coordinates, we check the rest of
      them to ensure that the GeoJSON is valid.

      Remember to call set_alias(), so that this wrapper does not take ownership
      of the data.
    */
    Json_wrapper coord((*coordinates)[i]);
    coord.set_alias();
    if (i == 0)
      point->set<0>(coord.coerce_real(""));
    else if (i == 1)
      point->set<1>(coord.coerce_real(""));
  }

  return false;
}


/**
  Takes a JSON array as input, does a recursive parsing and returns a
  Geometry object.

  This function differs from parse_object() in that it takes an array as input
  instead of a object. This is one of the members "coordinates" or "geometries"
  of a GeoJSON object.

  @param data_array A JSON array to parse.
  @param type The type of the GeoJSON object this array belongs to.
  @param rollback Pointer to a boolean indicating if parsed data should
         be reverted/rolled back.
  @param buffer A String buffer to be used by GeometryCollection.
  @param is_parent_featurecollection Indicating if the current geometry is a
         child of a FeatureCollection.
  @param[out] geometry A pointer to the parsed Geometry.

  @return true on failure, false otherwise.
*/
bool Item_func_geomfromgeojson::
parse_object_array(const Json_array *data_array, Geometry::wkbType type,
                   bool *rollback, String *buffer,
                   bool is_parent_featurecollection, Geometry **geometry)
{
  switch (type)
  {
  case Geometry::wkb_geometrycollection:
    {
      /*
        Ensure that the provided buffer is empty, and then create a empty
        GeometryCollection using this buffer.
      */
      buffer->set_charset(&my_charset_bin);
      buffer->length(0);
      buffer->reserve(GEOM_HEADER_SIZE + SIZEOF_INT);
      write_geometry_header(buffer, 0, Geometry::wkb_geometrycollection, 0);

      Gis_geometry_collection *collection= new Gis_geometry_collection();
      *geometry= collection;

      collection->set_data_ptr(buffer->ptr() + GEOM_HEADER_SIZE, 4);
      collection->has_geom_header_space(true);

      for (size_t i= 0; i < data_array->size(); ++i)
      {
        if ((*data_array)[i]->json_type() != enum_json_type::J_OBJECT)
        {
          my_error(ER_INVALID_GEOJSON_WRONG_TYPE, MYF(0), func_name(),
                   GEOMETRIES_MEMBER, "object array");
          return true;
        }

        const Json_object *object=
          down_cast<const Json_object*>((*data_array)[i]);

        String geo_buffer;
        Geometry *parsed_geometry= NULL;
        if (parse_object(object, rollback, &geo_buffer,
                         is_parent_featurecollection, &parsed_geometry))
        {
          /*
            This will happen if a feature object contains a NULL geometry
            object (which is a perfectly valid GeoJSON object).
          */
          if (*rollback)
          {
            *rollback= false;
          }
          else
          {
            delete parsed_geometry;
            parsed_geometry= NULL;

            return true;
          }
        }
        else
        {
          if (parsed_geometry->get_geotype() == Geometry::wkb_polygon)
          {
            // Make the Gis_polygon suitable for MySQL GIS code.
            Gis_polygon *polygon= static_cast<Gis_polygon*>(parsed_geometry);
            polygon->to_wkb_unparsed();
          }
          collection->append_geometry(parsed_geometry, buffer);
        }
        delete parsed_geometry;
        parsed_geometry= NULL;
      }
      return false;
    }
  case Geometry::wkb_point:
    {
      Gis_point *point= new Gis_point(false);
      *geometry= point;
      return get_positions(data_array, point);
    }
  case Geometry::wkb_linestring:
    {
      Gis_line_string *linestring= new Gis_line_string(false);
      *geometry= linestring;

      if (get_linestring(data_array, linestring))
        return true;
      return false;
    }
  case Geometry::wkb_multipoint:
    {
      // Ensure that the MultiPoint has at least one Point.
      if (data_array->size() == 0)
      {
        my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
        return true;
      }

      Gis_multi_point *multipoint= new Gis_multi_point(false);
      *geometry= multipoint;

      for (size_t i= 0; i < data_array->size(); ++i)
      {
        if ((*data_array)[i]->json_type() != enum_json_type::J_ARRAY)
        {
          my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
          return true;
        }
        else
        {
          const Json_array *coords=
            down_cast<const Json_array*>((*data_array)[i]);
          Gis_point point;
          if (get_positions(coords, &point))
            return true;
          multipoint->push_back(point);
        }
      }
      return false;
    }
  case Geometry::wkb_multilinestring:
    {
      // Ensure that the MultiLineString has at least one LineString.
      if (data_array->size() == 0)
      {
        my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
        return true;
      }

      Gis_multi_line_string *multilinestring= new Gis_multi_line_string(false);
      *geometry= multilinestring;
      for (size_t i= 0; i < data_array->size(); ++i)
      {
        if ((*data_array)[i]->json_type() != enum_json_type::J_ARRAY)
        {
          my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
          return true;
        }

        const Json_array *coords=
          down_cast<const Json_array*>((*data_array)[i]);
        Gis_line_string linestring;
        if (get_linestring(coords, &linestring))
          return true;
        multilinestring->push_back(linestring);
      }
      return false;
    }
  case Geometry::wkb_polygon:
    {
      Gis_polygon *polygon= new Gis_polygon(false);
      *geometry= polygon;
      return get_polygon(data_array, polygon);
    }
  case Geometry::wkb_multipolygon:
    {
      // Ensure that the MultiPolygon has at least one Polygon.
      if (data_array->size() == 0)
      {
        my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
        return true;
      }

      Gis_multi_polygon *multipolygon= new Gis_multi_polygon(false);
      *geometry= multipolygon;

      for (size_t i= 0; i < data_array->size(); ++i)
      {
        if ((*data_array)[i]->json_type() != enum_json_type::J_ARRAY)
        {
          my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
          return true;
        }

        const Json_array *coords=
          down_cast<const Json_array*>((*data_array)[i]);
        Gis_polygon polygon;
        if (get_polygon(coords, &polygon))
          return true;
        multipolygon->push_back(polygon);
      }
      return false;
    }
  default:
    {
      DBUG_ASSERT(false);
      return false;
    }
  }
}


/**
  Create a Gis_line_string from a JSON array.

  @param data_array A JSON array containing the coordinates.
  @param linestring Pointer to a linestring to be filled with data.

  @return true on failure, false otherwise.
*/
bool
Item_func_geomfromgeojson::get_linestring(const Json_array *data_array,
                                          Gis_line_string *linestring)
{
  // Ensure that the linestring has at least one point.
  if (data_array->size() < 2)
  {
    my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
    return true;
  }

  for (size_t i= 0; i < data_array->size(); ++i)
  {
    if ((*data_array)[i]->json_type() != enum_json_type::J_ARRAY)
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }
    else
    {
      Gis_point point;
      const Json_array *coords= down_cast<const Json_array*>((*data_array)[i]);
      if (get_positions(coords, &point))
        return true;
      linestring->push_back(point);
    }
  }

  return false;
}


/**
  Create a Gis_polygon from a JSON array.

  @param data_array A JSON array containing the coordinates.
  @param polygon A pointer to a Polygon to be filled with data.

  @return true on failure, false otherwise.
*/
bool Item_func_geomfromgeojson::get_polygon(const Json_array *data_array,
                                            Gis_polygon *polygon)
{
  // Ensure that the Polygon has at least one ring.
  if (data_array->size() == 0)
  {
    my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
    return true;
  }

  for (size_t ring_count= 0; ring_count < data_array->size(); ++ring_count)
  {
    // Polygon rings must have at least four points, according to GeoJSON spec.
    if ((*data_array)[ring_count]->json_type() != enum_json_type::J_ARRAY)
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }

    const Json_array *polygon_ring=
      down_cast<const Json_array*>((*data_array)[ring_count]);
    if (polygon_ring->size() < 4)
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }

    polygon->inners().resize(ring_count);
    for (size_t i= 0; i < polygon_ring->size(); ++i)
    {
      if ((*polygon_ring)[i]->json_type() != enum_json_type::J_ARRAY)
      {
        my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
        return true;
      }

      Gis_point point;
      const Json_array *coords=
        down_cast<const Json_array*>((*polygon_ring)[i]);
      if (get_positions(coords, &point))
        return true;

      if (ring_count == 0)
        polygon->outer().push_back(point);
      else
        polygon->inners()[ring_count - 1].push_back(point);
    }

    // Check if the ring is closed, which is must be according to GeoJSON spec.
    Gis_point first;
    Gis_point last;
    if (ring_count == 0)
    {
      first= polygon->outer()[0];
      last= polygon->outer().back();
    }
    else
    {
      first= polygon->inners()[ring_count - 1][0];
      last= polygon->inners()[ring_count - 1].back();
    }

    if (!(first == last))
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }
  }
  return false;
}


/**
  Converts GeoJSON type string to a wkbType.

  Convert a string from a "type" member in GeoJSON to its equivalent Geometry
  enumeration. The type names are case sensitive as stated in the specification:

    The value of the type member must be one of: "Point", "MultiPoint",
    "LineString", "MultiLineString", "Polygon", "MultiPolygon",
    "GeometryCollection", "Feature", or "FeatureCollection". The case of the
    type member values must be as shown here.

  Note that even though Feature and FeatureCollection are added here, these
  types will be handled before this function is called (in parse_object()).

  @param typestring A GeoJSON type string.

  @return The corresponding wkbType, or wkb_invalid_type if no matching
          type was found.
*/
Geometry::wkbType Item_func_geomfromgeojson::get_wkbtype(const char *typestring)
{
  if (strcmp(typestring, POINT_TYPE) == 0)
    return Geometry::wkb_point;
  else if (strcmp(typestring, MULTIPOINT_TYPE) == 0)
    return Geometry::wkb_multipoint;
  else if (strcmp(typestring, LINESTRING_TYPE) == 0)
    return Geometry::wkb_linestring;
  else if (strcmp(typestring, MULTILINESTRING_TYPE) == 0)
    return Geometry::wkb_multilinestring;
  else if (strcmp(typestring, POLYGON_TYPE) == 0)
    return Geometry::wkb_polygon;
  else if (strcmp(typestring, MULTIPOLYGON_TYPE) == 0)
    return Geometry::wkb_multipolygon;
  else if (strcmp(typestring, GEOMETRYCOLLECTION_TYPE) == 0)
    return Geometry::wkb_geometrycollection;
  else
    return Geometry::wkb_invalid_type;
}


/**
  Takes a GeoJSON CRS object as input and parses it into a SRID.

  If user has supplied a SRID, the parsing will be ignored.

  GeoJSON support two types of CRS objects; named and linked. Linked CRS will
  force us to download CRS parameters from the web, which we do not allow.
  Thus, we will only parse named CRS URNs in the "urn:ogc:def:crs:EPSG::<srid>"
  and "EPSG:<srid>" namespaces. In addition, "urn:ogc:def:crs:OGC:1.3:CRS84"
  will be recognized as SRID 4326. Note that CRS object with value JSON null is
  valid.

  @param crs_object A GeoJSON CRS object to parse.

  @return false if the parsing was successful, or true if it didn't understand
          the CRS object provided.
*/
bool Item_func_geomfromgeojson::
parse_crs_object(const Json_object *crs_object)
{
  if (m_user_provided_srid)
    return false;

  /*
    Check if required CRS members "type" and "properties" exists, and that they
    are of correct type according to GeoJSON specification.
  */
  const Json_dom *type_member= my_find_member_ncase(crs_object, TYPE_MEMBER);
  const Json_dom *properties_member= my_find_member_ncase(crs_object,
                                                          PROPERTIES_MEMBER);
  if (!is_member_valid(type_member, TYPE_MEMBER, enum_json_type::J_STRING,
                       false, NULL) ||
      !is_member_valid(properties_member, PROPERTIES_MEMBER,
                       enum_json_type::J_OBJECT, false, NULL))
  {
    return true;
  }

  // Check that this CRS is a named CRS, and not a linked CRS.
  const Json_string *type_member_str=
    down_cast<const Json_string*>(type_member);
  if (native_strcasecmp(type_member_str->value().c_str(), NAMED_CRS) != 0)
  {
    // CRS object is not a named CRS.
    my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
    return true;
  }

  /*
    Check that CRS properties member has the required member "name"
    of type "string".
  */
  const Json_object *properties_member_obj=
    down_cast<const Json_object*>(properties_member);
  const Json_dom *crs_name_member= my_find_member_ncase(properties_member_obj,
                                                        CRS_NAME_MEMBER);
  if (!is_member_valid(crs_name_member, CRS_NAME_MEMBER,
                       enum_json_type::J_STRING, false, NULL))
  {
    return true;
  }
  /*
    Now we can do the parsing of named CRS. The parsing happens as follows:

    1) Check if the named CRS is equal to urn:ogc:def:crs:OGC:1.3:CRS84". If so,
       return SRID 4326.
    2) Otherwise, check if we have a short or long format CRS URN in the
       EPSG namespace.
    3) If we have a CRS URN in the EPSG namespace, check if the ending after the
       last ':' is a valid SRID ("EPSG:<srid>" or
       "urn:ogc:def:crs:EPSG::<srid>"). An valid SRID must be greater than zero,
       and less than or equal to UINT_MAX32.
    4) If a SRID was returned from the parsing, check if we already have found
       a valid CRS earlier in the parsing. If so, and the SRID from the earlier
       CRS was different than the current, return an error to the user.

    If any of these fail, an error is returned to the user.
  */
  const Json_string *crs_name_member_str=
    down_cast<const Json_string*>(crs_name_member);
  longlong parsed_srid= -1;
  if (native_strcasecmp(crs_name_member_str->value().c_str(), CRS84_URN) == 0)
  {
    parsed_srid= 4326;
  }
  else
  {
    size_t start_index;
    size_t name_length= crs_name_member_str->size();
    const char *crs_name= crs_name_member_str->value().c_str();
    if (native_strncasecmp(crs_name, SHORT_EPSG_PREFIX, 5) == 0)
    {
      start_index= 5;
    }
    else if (native_strncasecmp(crs_name, LONG_EPSG_PREFIX, 22) == 0)
    {
      start_index= 22;
    }
    else
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }

    char *end_of_parse;
    longlong parsed_value= strtoll(crs_name + start_index, &end_of_parse, 10);

    /*
      Check that the whole ending got parsed, and that the value is within
      valid SRID range.
    */
    if (end_of_parse == (crs_name + name_length) && parsed_value > 0 &&
        parsed_value <= UINT_MAX32)
    {
      parsed_srid= static_cast<uint32>(parsed_value);
    }
    else
    {
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }
  }

  if (parsed_srid > 0)
  {
    if (m_srid_found_in_document > 0 && parsed_srid != m_srid_found_in_document)
    {
      // A SRID has already been found, which had a different value.
      my_error(ER_INVALID_GEOJSON_UNSPECIFIED, MYF(0), func_name());
      return true;
    }
    else
    {
      m_srid_found_in_document= parsed_srid;
    }
  }
  return false;
}


/**
  Checks if a JSON member is valid based on input criteria.

  This function checks if the provided member exists, and if it's of the
  expected type. If it fails ome of the test, my_error() is called and false is
  returned from the function.

  @param member The member to validate.
  @param member_name Name of the member we are validating, so that the error
         returned to the user is more informative.
  @param expected_type Expected type of the member.
  @param allow_null If we shold allow the member to have JSON null value.
  @param[out] was_null This will be set to true if the provided member had a
              JSON null value. Is only affected if allow_null is set to true.

  @return true if the member is valid, false otherwise.
*/
bool Item_func_geomfromgeojson::
is_member_valid(const Json_dom *member, const char *member_name,
                enum_json_type expected_type, bool allow_null,
                bool *was_null)
{
  if (member == NULL)
  {
    my_error(ER_INVALID_GEOJSON_MISSING_MEMBER, MYF(0), func_name(),
             member_name);
    return false;
  }

  if (allow_null)
  {
    DBUG_ASSERT(was_null != NULL);
    *was_null= member->json_type() == enum_json_type::J_NULL;
    if (*was_null)
      return true;
  }

  const char *type_name;
  if (member->json_type() != expected_type)
  {
    switch (expected_type)
    {
    case enum_json_type::J_OBJECT:
      type_name= "object";
      break;
    case enum_json_type::J_ARRAY:
      type_name= "array";
      break;
    case enum_json_type::J_STRING:
      type_name= "string";
      break;
    default:
      /* purecov: begin deadcode */
      DBUG_ASSERT(false);
      return false;
      /* purecov: end */
    }

    my_error(ER_INVALID_GEOJSON_WRONG_TYPE, MYF(0), func_name(), member_name,
             type_name);
    return false;
  }
  return true;
}


/**
  Checks if the supplied argument is a valid integer type.

  The function will fail if the supplied data is binary data. It will accept
  strings as integer type. Used for checking SRID and OPTIONS argument.

  @param argument The argument to check.

  @return true if the argument is a valid integer type, false otherwise.
*/
bool Item_func_geomfromgeojson::check_argument_valid_integer(Item *argument)
{
  bool is_binary_charset= (argument->collation.collation == &my_charset_bin);
  bool is_parameter_marker= (argument->type() == PARAM_ITEM);

  switch (argument->field_type())
  {
  case MYSQL_TYPE_NULL:
    return true;
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
    return (!is_binary_charset || is_parameter_marker);
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_TINY:
    return true;
  default:
    return false;
  }
}


/// Do type checking on all provided arguments.
bool Item_func_geomfromgeojson::fix_fields(THD *thd, Item **ref)
{
  if (Item_geometry_func::fix_fields(thd, ref))
    return true;

  switch (arg_count)
  {
  case 3:
    {
      // Validate SRID argument
      if (!check_argument_valid_integer(args[2]))
      {
        my_error(ER_INCORRECT_TYPE, MYF(0), "SRID", func_name());
        return true;
      }
    }
  case 2:
    {
      // Validate options argument
      if (!check_argument_valid_integer(args[1]))
      {
        my_error(ER_INCORRECT_TYPE, MYF(0), "options", func_name());
        return true;
      }
    }
  case 1:
    {
      /*
        Validate GeoJSON argument type. We do not allow binary data as GeoJSON
        argument.
      */
      bool is_binary_charset= (args[0]->collation.collation == &my_charset_bin);
      bool is_parameter_marker= (args[0]->type() == PARAM_ITEM);
      switch (args[0]->field_type())
      {
      case MYSQL_TYPE_NULL:
        break;
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
        if (is_binary_charset && !is_parameter_marker)
        {
          my_error(ER_INCORRECT_TYPE, MYF(0), "geojson", func_name());
          return true;
        }
        break;
      default:
        my_error(ER_INCORRECT_TYPE, MYF(0), "geojson", func_name());
        return true;
      }
      break;
    }
  }

  /*
    Set maybe_null always to true. This is because the following GeoJSON input
    will return SQL NULL:

      {
        "type": "Feature",
        "geometry": null,
        "properties": { "name": "Foo Bar" }
      }
  */
  maybe_null= true;
  return false;
}


/**
  Converts a wkbType to the corresponding GeoJSON type.

  @param type The WKB Type to convert.

  @return The corresponding GeoJSON type, or NULL if no such type exists.
*/
static const char *
wkbtype_to_geojson_type(Geometry::wkbType type)
{
  switch (type)
  {
  case Geometry::wkb_geometrycollection:
    return Item_func_geomfromgeojson::GEOMETRYCOLLECTION_TYPE;
  case Geometry::wkb_point:
    return Item_func_geomfromgeojson::POINT_TYPE;
  case Geometry::wkb_multipoint:
    return Item_func_geomfromgeojson::MULTIPOINT_TYPE;
  case Geometry::wkb_linestring:
    return Item_func_geomfromgeojson::LINESTRING_TYPE;
  case Geometry::wkb_multilinestring:
    return Item_func_geomfromgeojson::MULTILINESTRING_TYPE;
  case Geometry::wkb_polygon:
    return Item_func_geomfromgeojson::POLYGON_TYPE;
  case Geometry::wkb_multipolygon:
    return Item_func_geomfromgeojson::MULTIPOLYGON_TYPE;
  case Geometry::wkb_invalid_type:
  case Geometry::wkb_polygon_inner_rings:
  default:
    return NULL;
  }
}


/**
  Append a GeoJSON array with coordinates to the writer at the current position.

  The WKB parser must be positioned at the beginning of the coordinates.
  There must exactly two coordinates in the array (x and y). The coordinates are
  rounded to the number of decimals specified in the variable
  max_decimal_digits.:

  max_decimal_digits == 2: 12.789 => 12.79
                           10     => 10.00

  @param parser WKB parser with position set to the beginning of the
         coordinates.
  @param array JSON array to append the result to.
  @param mbr A bounding box, which will be updated with data from the
         coordinates.
  @param calling_function Name of user-invoked function
  @param max_decimal_digits Max length of decimal numbers
  @param add_bounding_box True if a bounding box should be added
  @param add_short_crs_urn True if we should add short format CRS URN
  @param add_long_crs_urn True if we should add long format CRS URN
  @param geometry_srid Spacial Reference System Identifier being used

  @return false on success, true otherwise.
*/
static bool
append_coordinates(Geometry::wkb_parser *parser, Json_array *array, MBR *mbr,
                   const char *calling_function,
                   int max_decimal_digits,
                   bool add_bounding_box,
                   bool add_short_crs_urn,
                   bool add_long_crs_urn,
                   uint32 geometry_srid)
{
  point_xy coordinates;
  if (parser->scan_xy(&coordinates))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
    return true;
  }

  double x_value=
    my_double_round(coordinates.x, max_decimal_digits, true, false);
  double y_value=
    my_double_round(coordinates.y, max_decimal_digits, true, false);

  if (array->append_alias(new (std::nothrow) Json_double(x_value)) ||
      array->append_alias(new (std::nothrow) Json_double(y_value)))
  {
    return true;
  }

  if (add_bounding_box)
    mbr->add_xy(x_value, y_value);
  return false;
}


/**
  Append a GeoJSON LineString object to the writer at the current position.

  The parser must be positioned after the LineString header, and there must be
  at least one coordinate array in the linestring.

  @param parser WKB parser with position set to after the LineString header.
  @param points JSON array to append the result to.
  @param mbr A bounding box, which will be updated with data from the
             LineString.
  @param calling_function Name of user-invoked function
  @param max_decimal_digits Max length of decimal numbers
  @param add_bounding_box True if a bounding box should be added
  @param add_short_crs_urn True if we should add short format CRS URN
  @param add_long_crs_urn True if we should add long format CRS URN
  @param geometry_srid Spacial Reference System Identifier being used

  @return false on success, true otherwise.
*/
static bool
append_linestring(Geometry::wkb_parser *parser, Json_array *points, MBR *mbr,
                  const char *calling_function,
                  int max_decimal_digits,
                  bool add_bounding_box,
                  bool add_short_crs_urn,
                  bool add_long_crs_urn,
                  uint32 geometry_srid)
{
  uint32 num_points= 0;
  if (parser->scan_non_zero_uint4(&num_points))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
    return true;
  }

  while (num_points--)
  {
    Json_array *point= new (std::nothrow) Json_array();
    if (point == NULL || points->append_alias(point) ||
        append_coordinates(parser, point, mbr, calling_function,
                           max_decimal_digits,
                           add_bounding_box,
                           add_short_crs_urn,
                           add_long_crs_urn,
                           geometry_srid))
    {
      return true;
    }
  }

  return false;
}


/**
  Append a GeoJSON Polygon object to the writer at the current position.

  The parser must be positioned after the Polygon header, and all coordinate
  arrays must contain at least one value.

  @param parser WKB parser with position set to after the Polygon header.
  @param polygon_rings JSON array to append the result to.
  @param mbr A bounding box, which will be updated with data from the Polygon.
  @param calling_function Name of user-invoked function
  @param max_decimal_digits Max length of decimal numbers
  @param add_bounding_box True if a bounding box should be added
  @param add_short_crs_urn True if we should add short format CRS URN
  @param add_long_crs_urn True if we should add long format CRS URN
  @param geometry_srid Spacial Reference System Identifier being used

  @return false on success, true otherwise.
*/
static
bool append_polygon(Geometry::wkb_parser *parser,
                    Json_array *polygon_rings, MBR *mbr, const char *calling_function,
                    int max_decimal_digits,
                    bool add_bounding_box,
                    bool add_short_crs_urn,
                    bool add_long_crs_urn,
                    uint32 geometry_srid)
{
  uint32 num_inner_rings= 0;
  if (parser->scan_non_zero_uint4(&num_inner_rings))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
    return true;
  }

  while (num_inner_rings--)
  {
    Json_array *polygon_ring= new (std::nothrow) Json_array();
    if (polygon_ring == NULL || polygon_rings->append_alias(polygon_ring))
      return true;

    uint32 num_points= 0;
    if (parser->scan_non_zero_uint4(&num_points))
    {
      my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
      return true;
    }

    while (num_points--)
    {
      Json_array *point = new (std::nothrow) Json_array();
      if (point == NULL || polygon_ring->append_alias(point) ||
          append_coordinates(parser, point, mbr, calling_function,
                             max_decimal_digits,
                             add_bounding_box,
                             add_short_crs_urn,
                             add_long_crs_urn,
                             geometry_srid))
      {
        return true;
      }
    }
  }

  return false;
}


/**
  Appends a GeoJSON bounding box to the rapidjson output buffer.

  @param mbr Bounding box to write.
  @param geometry The JSON object to append the bounding box to.

  @return false on success, true otherwise.
*/
static bool append_bounding_box(MBR *mbr, Json_object *geometry)
{
  DBUG_ASSERT(GEOM_DIM == 2);

  Json_array *bbox_array= new (std::nothrow) Json_array();
  if (bbox_array == NULL ||
      geometry->add_alias("bbox", bbox_array) ||
      bbox_array->append_alias(new (std::nothrow) Json_double(mbr->xmin)) ||
      bbox_array->append_alias(new (std::nothrow) Json_double(mbr->ymin)) ||
      bbox_array->append_alias(new (std::nothrow) Json_double(mbr->xmax)) ||
      bbox_array->append_alias(new (std::nothrow) Json_double(mbr->ymax)))
  {
    return true;
  }

  return false;
}


/**
  Appends a GeoJSON CRS object to the rapidjson output buffer.

  If both add_long_crs_urn and add_short_crs_urn is specified, the long CRS URN
  is preferred as mentioned in the GeoJSON specification:

  "OGC CRS URNs such as "urn:ogc:def:crs:OGC:1.3:CRS84" shall be preferred
  over legacy identifiers such as "EPSG:4326""

  @param geometry The JSON object to append the CRS object to.
  @param max_decimal_digits Max length of decimal numbers
  @param add_bounding_box True if a bounding box should be added
  @param add_short_crs_urn True if we should add short format CRS URN
  @param add_long_crs_urn True if we should add long format CRS URN
  @param geometry_srid Spacial Reference System Identifier being used

  @return false on success, true otherwise.
*/
static bool append_crs(Json_object *geometry,
                       int max_decimal_digits,
                       bool add_bounding_box,
                       bool add_short_crs_urn,
                       bool add_long_crs_urn,
                       uint32 geometry_srid)
{
  DBUG_ASSERT(add_long_crs_urn || add_short_crs_urn);
  DBUG_ASSERT(geometry_srid > 0);

  Json_object *crs_object= new (std::nothrow) Json_object();
  if (crs_object == NULL || geometry->add_alias("crs", crs_object) ||
      crs_object->add_alias("type", new (std::nothrow) Json_string("name")))
  {
    return true;
  }

  Json_object *crs_properties= new (std::nothrow) Json_object();
  if (crs_properties == NULL ||
      crs_object->add_alias("properties", crs_properties))
  {
    return true;
  }

  // Max width of SRID + '\0'
  char srid_string[MAX_INT_WIDTH + 1];
  llstr(geometry_srid, srid_string);

  char crs_name[MAX_CRS_WIDTH];
  if (add_long_crs_urn)
    strcpy(crs_name, Item_func_geomfromgeojson::LONG_EPSG_PREFIX);
  else if (add_short_crs_urn)
    strcpy(crs_name, Item_func_geomfromgeojson::SHORT_EPSG_PREFIX);

  strcat(crs_name, srid_string);
  if (crs_properties->add_alias("name",
                                new (std::nothrow) Json_string(crs_name)))
  {
    return true;
  }

  return false;
}


/**
  Reads a WKB GEOMETRY from input and writes the equivalent GeoJSON to the
  output. If a GEOMETRYCOLLECTION is found, this function will call itself for
  each GEOMETRY in the collection.

  @param parser The WKB input to read from, positioned at the start of
         GEOMETRY header.
  @param geometry JSON object to append the result to.
  @param is_root_object Indicating if the current GEOMETRY is the root object
         in the output GeoJSON.
  @param mbr A bounding box, which will be updated with data from all the
         GEOMETRIES found in the input.
  @param calling_function Name of the user-invoked function
  @param max_decimal_digits Max length of decimal numbers
  @param add_bounding_box True if a bounding box should be added
  @param add_short_crs_urn True if we should add short format CRS URN
  @param add_long_crs_urn True if we should add long format CRS URN
  @param geometry_srid Spacial Reference System Identifier being used

  @return false on success, true otherwise.
*/
static bool
append_geometry(Geometry::wkb_parser *parser, Json_object *geometry,
                bool is_root_object, MBR *mbr, const char *calling_function,
                int max_decimal_digits,
                bool add_bounding_box,
                bool add_short_crs_urn,
                bool add_long_crs_urn,
                uint32 geometry_srid)
{
  // Check of wkb_type is within allowed range.
  wkb_header header;
  if (parser->scan_wkb_header(&header) ||
      header.wkb_type < Geometry::wkb_first ||
      header.wkb_type > Geometry::wkb_geometrycollection)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
    return true;
  }

  const char *geojson_type=
    wkbtype_to_geojson_type(static_cast<Geometry::wkbType>(header.wkb_type));
  if (geometry->add_alias("type", new (std::nothrow) Json_string(geojson_type)))
    return true;

  /*
    Use is_mbr_empty to check if we encounter any empty GEOMETRY collections.
    In that case, we don't want to write a bounding box to the GeoJSON output.
  */
  bool is_mbr_empty= false;

  switch (header.wkb_type)
  {
  case Geometry::wkb_point:
    {
      Json_array *point= new (std::nothrow) Json_array();
      if (point == NULL || geometry->add_alias("coordinates", point) ||
          append_coordinates(parser, point, mbr, calling_function,
                             max_decimal_digits,
                             add_bounding_box,
                             add_short_crs_urn,
                             add_long_crs_urn,
                             geometry_srid))
      {
        return true;
      }
      break;
    }
  case Geometry::wkb_linestring:
    {
      Json_array *points= new (std::nothrow) Json_array();
      if (points == NULL || geometry->add_alias("coordinates", points) ||
          append_linestring(parser, points, mbr, calling_function,
                            max_decimal_digits,
                            add_bounding_box,
                            add_short_crs_urn,
                            add_long_crs_urn,
                            geometry_srid))
      {
        return true;
      }
      break;
    }
  case Geometry::wkb_polygon:
    {
      Json_array *polygon_rings= new (std::nothrow) Json_array();
      if (polygon_rings == NULL ||
          geometry->add_alias("coordinates", polygon_rings) ||
          append_polygon(parser, polygon_rings, mbr, calling_function,
                         max_decimal_digits,
                         add_bounding_box,
                         add_short_crs_urn,
                         add_long_crs_urn,
                         geometry_srid))
      {
        return true;
      }
      break;
    }
  case Geometry::wkb_multipoint:
  case Geometry::wkb_multipolygon:
  case Geometry::wkb_multilinestring:
    {
      uint32 num_items= 0;
      if (parser->scan_non_zero_uint4(&num_items))
      {
        my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
        return true;
      }

      Json_array *collection= new (std::nothrow) Json_array();
      if (collection == NULL || geometry->add_alias("coordinates", collection))
        return true;

      while (num_items--)
      {
        if (parser->skip_wkb_header())
        {
          my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
          return true;
        }
        else
        {
          bool result;
          Json_array *points= new (std::nothrow) Json_array();
          if (points == NULL || collection->append_alias(points))
            return true;

          if (header.wkb_type == Geometry::wkb_multipoint)
            result= append_coordinates(parser, points, mbr, calling_function,
                                       max_decimal_digits,
                                       add_bounding_box,
                                       add_short_crs_urn,
                                       add_long_crs_urn,
                                       geometry_srid);
          else if (header.wkb_type == Geometry::wkb_multipolygon)
            result= append_polygon(parser, points, mbr, calling_function,
                                   max_decimal_digits,
                                   add_bounding_box,
                                   add_short_crs_urn,
                                   add_long_crs_urn,
                                   geometry_srid);
          else if (Geometry::wkb_multilinestring)
            result= append_linestring(parser, points, mbr, calling_function,
                                      max_decimal_digits,
                                      add_bounding_box,
                                      add_short_crs_urn,
                                      add_long_crs_urn,
                                      geometry_srid);
          else
            DBUG_ASSERT(false);

          if (result)
            return true;
        }
      }
      break;
    }
  case Geometry::wkb_geometrycollection:
    {
      uint32 num_geometries= 0;
      if (parser->scan_uint4(&num_geometries))
      {
        my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
        return true;
      }

      is_mbr_empty= (num_geometries == 0);

      Json_array *collection= new (std::nothrow) Json_array();
      if (collection == NULL || geometry->add_alias("geometries", collection))
        return true;

      while (num_geometries--)
      {
        // Create a new MBR for the collection.
        MBR subcollection_mbr;
        Json_object *sub_geometry= new (std::nothrow) Json_object();
        if (sub_geometry == NULL || collection->append_alias(sub_geometry) ||
            append_geometry(parser, sub_geometry, false, &subcollection_mbr, calling_function,
                            max_decimal_digits,
                            add_bounding_box,
                            add_short_crs_urn,
                            add_long_crs_urn,
                            geometry_srid))
        {
          return true;
        }

        if (add_bounding_box)
          mbr->add_mbr(&subcollection_mbr);
      }
      break;
    }
  default:
    {
      // This should not happen, since we did a check on wkb_type earlier.
      /* purecov: begin inspected */
      DBUG_ASSERT(false);
      return true;
      /* purecov: end inspected */
    }
  }

  // Only add a CRS object if the SRID of the GEOMETRY is not 0.
  if (is_root_object && (add_long_crs_urn || add_short_crs_urn) &&
      geometry_srid > 0)
  {
    append_crs(geometry,
               max_decimal_digits,
               add_bounding_box,
               add_short_crs_urn,
               add_long_crs_urn,
               geometry_srid);
  }

  if (add_bounding_box && !is_mbr_empty)
    append_bounding_box(mbr, geometry);

  return false;
}

/** The contract for this function is found in item_json_func.h */
bool geometry_to_json(Json_wrapper *wr, Item *geometry_arg, const char *calling_function,
                      int max_decimal_digits,
                      bool add_bounding_box,
                      bool add_short_crs_urn,
                      bool add_long_crs_urn,
                      uint32 *geometry_srid)
{
  String arg_val;
  String *swkb= geometry_arg->val_str(&arg_val);
  if ((geometry_arg->null_value))
    return false;

  Geometry::wkb_parser parser(swkb->ptr(), swkb->ptr() + swkb->length());
  if (parser.scan_uint4(geometry_srid))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), calling_function);
    return true;
  }

  /*
    append_geometry() will go through the WKB and call itself recursivly if
    geometry collections are encountered. For each recursive call, a new MBR
    is created. The function will fail if it encounters invalid data in the
    WKB input.
  */
  MBR mbr;
  Json_object *geojson_object= new (std::nothrow) Json_object();

  if (geojson_object == NULL ||
      append_geometry(&parser, geojson_object, true, &mbr, calling_function,
                      max_decimal_digits,
                      add_bounding_box,
                      add_short_crs_urn,
                      add_long_crs_urn,
                      *geometry_srid))
  {
    delete geojson_object;
    return true;
  }

  Json_wrapper w(geojson_object);
  wr->steal(&w);
  return false;
}

/**
  Create a GeoJSON object, according to GeoJSON specification revison 1.0.
*/
bool Item_func_as_geojson::val_json(Json_wrapper *wr)
{
  DBUG_ASSERT(fixed == TRUE);

  if ((arg_count > 1 && parse_maxdecimaldigits_argument()) ||
      (arg_count > 2 && parse_options_argument()))
  {
    if (null_value && !current_thd->is_error())
      return false;
    else
      return error_json();
  }

  /*
    Set maximum number of decimal digits. If maxdecimaldigits argument was not
    specified, set unlimited number of decimal digits.
  */
  if (arg_count < 2)
    m_max_decimal_digits= INT_MAX32;

  if (geometry_to_json(wr, args[0], func_name(),
                       m_max_decimal_digits,
                       m_add_bounding_box,
                       m_add_short_crs_urn,
                       m_add_long_crs_urn,
                       &m_geometry_srid))
  {
    if (null_value && !current_thd->is_error())
      return false;
    else
      return error_json();
  }

  null_value= args[0]->null_value;
  return false;
}


/**
  Parse the value in options argument.

  Options is a 3-bit bitmask with the following options:

  0  No options (default values).
  1  Add a bounding box to the output.
  2  Add a short CRS URN to the output. The default format is a
     short format ("EPSG:<srid>").
  4  Add a long format CRS URN ("urn:ogc:def:crs:EPSG::<srid>"). This
     will override option 2. E.g., bitmask 5 and 7 mean the
     same: add a bounding box and a long format CRS URN.

  If value is out of range (below zero or greater than seven), an error will be
  raised. This function expects that the options argument is the third argument
  in the function call.

  @return false on success, true otherwise (value out of range or similar).
*/
bool Item_func_as_geojson::parse_options_argument()
{
  DBUG_ASSERT(arg_count > 2);
  longlong options_argument= args[2]->val_int();
  if ((null_value= args[2]->null_value))
    return true;

  if (options_argument < 0 || options_argument > 7)
  {
    char options_string[MAX_BIGINT_WIDTH + 1];
    if (args[2]->unsigned_flag)
      ullstr(options_argument, options_string);
    else
      llstr(options_argument, options_string);

    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "options", options_string,
             func_name());
    return true;
  }

  m_add_bounding_box= options_argument & (1 << 0);
  m_add_short_crs_urn= options_argument & (1 << 1);
  m_add_long_crs_urn= options_argument & (1 << 2);

  if (m_add_long_crs_urn)
    m_add_short_crs_urn= false;
  return false;
}


/**
  Parse the value in maxdecimaldigits argument.

  This value MUST be a positive integer. If value is out of range (negative
  value or greater than INT_MAX), an error will be raised. This function expects
  that the maxdecimaldigits argument is the second argument in the function
  call.

  @return false on success, true otherwise (negative value or similar).
*/
bool Item_func_as_geojson::parse_maxdecimaldigits_argument()
{
  DBUG_ASSERT(arg_count > 1);
  longlong max_decimal_digits_argument = args[1]->val_int();
  if ((null_value= args[1]->null_value))
    return true;

  if (max_decimal_digits_argument < 0 ||
      max_decimal_digits_argument > INT_MAX32)
  {
    char max_decimal_digits_string[MAX_BIGINT_WIDTH + 1];
    if (args[1]->unsigned_flag)
      ullstr(max_decimal_digits_argument, max_decimal_digits_string);
    else
      llstr(max_decimal_digits_argument, max_decimal_digits_string);

    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "max decimal digits",
             max_decimal_digits_string, func_name());
    return true;
  }

  m_max_decimal_digits= static_cast<int>(max_decimal_digits_argument);
  return false;
}


/**
  Perform type checking on all arguments.

    @<geometry@> argument must be a geometry.
    @<maxdecimaldigits@> must be an integer value.
    @<options@> must be an integer value.

  Set maybe_null to the correct value.
*/
bool Item_func_as_geojson::fix_fields(THD *thd, Item **ref)
{
  if (Item_json_func::fix_fields(thd, ref))
    return true;

  /*
    We must set maybe_null to true, since the GeoJSON string may be longer than
    the packet size.
  */
  maybe_null= true;

  // Check if the geometry argument is considered as a geometry type.
  if (!is_item_geometry_type(args[0]))
  {
    my_error(ER_INCORRECT_TYPE, MYF(0), "geometry", func_name());
    return true;
  }

  if (arg_count > 1)
  {
    if (!Item_func_geomfromgeojson::check_argument_valid_integer(args[1]))
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "max decimal digits", func_name());
      return true;
    }
  }

  if (arg_count > 2)
  {
    if (!Item_func_geomfromgeojson::check_argument_valid_integer(args[2]))
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "options", func_name());
      return true;
    }
  }
  return false;
}


/**
  Checks if supplied item is a valid latitude or longitude, based on which
  type it is. Implemented as a whitelist of allowed types, where binary data is
  not allowed.

  @param arg Item to check for valid latitude/longitude.
  @return false if item is not valid, true otherwise.
*/
bool Item_func_geohash::check_valid_latlong_type(Item *arg)
{
  if (is_item_null(arg))
    return true;

  /*
    is_field_type_valid will be true if the item is a constant or a field of
    valid type.
  */
  bool is_binary_charset= (arg->collation.collation == &my_charset_bin);
  bool is_field_type_valid= false;
  switch (arg->field_type())
  {
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
    is_field_type_valid= !is_binary_charset;
    break;
  default:
    is_field_type_valid= false;
    break;
  }

  /*
    Parameters and parameter markers always have
    field_type() == MYSQL_TYPE_VARCHAR. type() is dependent on if it's a
    parameter marker or parameter (PREPARE or EXECUTE, respectively).
  */
  bool is_parameter= (arg->type() == INT_ITEM || arg->type() == DECIMAL_ITEM ||
                      arg->type() == REAL_ITEM || arg->type() == STRING_ITEM) &&
                     (arg->field_type() == MYSQL_TYPE_VARCHAR);
  bool is_parameter_marker= (arg->type() == PARAM_ITEM &&
                             arg->field_type() == MYSQL_TYPE_VARCHAR);

  if (is_field_type_valid || is_parameter_marker || is_parameter)
    return true;
  return false;
}


/**
  Populate member variables with values from arguments.

  In this function we populate the member variables 'latitude', 'longitude'
  and 'geohash_length' with values from the arguments supplied by the user.
  We also do type checking on the geometry object, as well as out-of-range
  check for both longitude, latitude and geohash length.

  If an expection is raised, null_value will not be set. If a null argument
  was detected, null_value will be set to true.

  @return false if class variables was populated, or true if the function
          failed to populate them.
*/
bool Item_func_geohash::fill_and_check_fields()
{
  longlong geohash_length_arg= -1;
  if (arg_count == 2)
  {
    // First argument is point, second argument is geohash output length.
    String string_buffer;
    String *swkb= args[0]->val_str(&string_buffer);
    geohash_length_arg= args[1]->val_int();

    if ((null_value= args[0]->null_value || args[1]->null_value || !swkb))
    {
      return true;
    }
    else
    {
      Geometry *geom;
      Geometry_buffer geometry_buffer;
      if (!(geom= Geometry::construct(&geometry_buffer, swkb)))
      {
        my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
        return true;
      }
      else if (geom->get_type() != Geometry::wkb_point ||
               geom->get_x(&longitude) || geom->get_y(&latitude))
      {
        my_error(ER_INCORRECT_TYPE, MYF(0), "point", func_name());
        return true;
      }
    }
  }
  else if (arg_count == 3)
  {
    /*
      First argument is longitude, second argument is latitude
      and third argument is geohash output length.
    */
    longitude= args[0]->val_real();
    latitude= args[1]->val_real();
    geohash_length_arg= args[2]->val_int();

    if ((null_value= args[0]->null_value || args[1]->null_value ||
         args[2]->null_value))
      return true;
  }

  // Check if supplied arguments are within allowed range.
  if (longitude > max_longitude || longitude < min_longitude)
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "longitude", func_name());
    return true;
  }
  else if (latitude > max_latitude || latitude < min_latitude)
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "latitude", func_name());
    return true;
  }

  if (geohash_length_arg <= 0 ||
      geohash_length_arg > upper_limit_output_length)
  {
    char geohash_length_string[MAX_BIGINT_WIDTH + 1];
    llstr(geohash_length_arg, geohash_length_string);
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "max geohash length", func_name());
    return true;
  }

  geohash_max_output_length= static_cast<uint>(geohash_length_arg);
  return false;
}


/**
  Encodes a pair of longitude and latitude values into a geohash string.
  The length of the output string will be no longer than the value of
  geohash_max_output_length member variable, but it might be shorter. The stop
  condition is the following:

  After appending a character to the output string, check if the encoded values
  of latitude and longitude matches the input arguments values. If so, return
  the result to the user.

  It does exist latitudes/longitude values which will cause the algorithm to
  loop until MAX(max_geohash_length, upper_geohash_limit), no matter how large
  these arguments are (eg. SELECT GEOHASH(0.01, 1, 100); ). It is thus
  important to supply an reasonable max geohash length argument.
*/
String *Item_func_geohash::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed == TRUE);

  if (fill_and_check_fields())
  {
    if (null_value)
    {
      return NULL;
    }
    else
    {
      /*
        Since null_value == false, my_error() was raised inside
        fill_and_check_fields().
      */
      return error_str();
    }
  }

  // Allocate one extra byte, for trailing '\0'.
  if (str->alloc(geohash_max_output_length + 1))
    return make_empty_result();
  str->length(0);

  double upper_latitude= max_latitude;
  double lower_latitude= min_latitude;
  double upper_longitude= max_longitude;
  double lower_longitude= min_longitude;
  bool even_bit= true;

  for (uint i= 0; i < geohash_max_output_length; i++)
  {
    /*
      We must encode in blocks of five bits, so we don't risk stopping
      in the middle of a character. If we stop in the middle of a character,
      some encoded geohash values will differ from geohash.org.
    */
    char current_char= 0;
    for (uint bit_number= 0; bit_number < 5; bit_number++)
    {
      if (even_bit)
      {
        // Encode one longitude bit.
        encode_bit(&upper_longitude, &lower_longitude, longitude,
                   &current_char, bit_number);
      }
      else
      {
        // Encode one latitude bit.
        encode_bit(&upper_latitude, &lower_latitude, latitude,
                   &current_char, bit_number);
      }
      even_bit = !even_bit;
    }
    str->q_append(char_to_base32(current_char));

    /*
      If encoded values of latitude and longitude matches the supplied
      arguments, there is no need to do more calculation.
    */
    if (latitude == (lower_latitude + upper_latitude) / 2.0 &&
        longitude == (lower_longitude + upper_longitude) / 2.0)
      break;
  }
  return str;
}


bool Item_func_geohash::resolve_type(THD *thd)
{
  fix_length_and_charset(Item_func_geohash::upper_limit_output_length,
                         default_charset());
  return false;
}


/**
  Here we check for valid types. We have to accept geometry of any type,
  and determine if it's really a POINT in val_str().
*/
bool Item_func_geohash::fix_fields(THD *thd, Item **ref)
{
  if (Item_str_func::fix_fields(thd, ref))
    return true;

  int geohash_length_arg_index;
  if (arg_count == 2)
  {
    /*
      First argument expected to be a point and second argument is expected
      to be geohash output length.
    */
    geohash_length_arg_index= 1;
    maybe_null= (args[0]->maybe_null || args[1]->maybe_null);
    if (!is_item_geometry_type(args[0]))
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "point", func_name());
      return true;
    }
  }
  else if (arg_count == 3)
  {
    /*
      First argument is expected to be longitude, second argument is expected
      to be latitude and third argument is expected to be geohash
      output length.
    */
    geohash_length_arg_index= 2;
    maybe_null= (args[0]->maybe_null || args[1]->maybe_null ||
                 args[2]->maybe_null);
    if (!check_valid_latlong_type(args[0]))
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "longitude", func_name());
      return true;
    }
    else if (!check_valid_latlong_type(args[1]))
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "latitude", func_name());
      return true;
    }
  }
  else
  {
    /*
      This should never happen, since function
      only supports two or three arguments.
    */
    DBUG_ASSERT(false);
    return true;
  }


  /*
    Check if geohash length argument is of valid type.

    PARAM_ITEM is to allow parameter marker during PREPARE, and INT_ITEM is to
    allow EXECUTE of prepared statements and usage of user-defined variables.
  */
  if (is_item_null(args[geohash_length_arg_index]))
    return false;

  bool is_binary_charset=
    (args[geohash_length_arg_index]->collation.collation == &my_charset_bin);
  bool is_parameter=
    (args[geohash_length_arg_index]->type() == PARAM_ITEM ||
     args[geohash_length_arg_index]->type() == INT_ITEM);

  switch (args[geohash_length_arg_index]->field_type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
    if (is_binary_charset && !is_parameter)
    {
      my_error(ER_INCORRECT_TYPE, MYF(0), "geohash max length", func_name());
      return true;
    }
    break;
  default:
    my_error(ER_INCORRECT_TYPE, MYF(0), "geohash max length", func_name());
    return true;
  }
  return false;
}


/**
  Sets the bit number in char_value, determined by following formula:

  IF target_value < (middle between lower_value and upper_value)
  set bit to 0
  ELSE
  set bit to 1

  When the function returns, upper_value OR lower_value are adjusted
  to the middle value between lower and upper.

  @param upper_value The upper error range for latitude or longitude.
  @param lower_value The lower error range for latitude or longitude.
  @param target_value Latitude or longitude value supplied as argument
  by the user.
  @param char_value The character we want to set the bit on.
  @param bit_number Wich bit number in char_value to set.
*/
void Item_func_geohash::encode_bit(double *upper_value, double *lower_value,
                                   double target_value, char *char_value,
                                   int bit_number)
{
  DBUG_ASSERT(bit_number >= 0 && bit_number <= 4);

  double middle_value= (*upper_value + *lower_value) / 2.0;
  if (target_value < middle_value)
  {
    *upper_value= middle_value;
    *char_value |= 0 << (4 - bit_number);
  }
  else
  {
    *lower_value= middle_value;
    *char_value |= 1 << (4 - bit_number);
  }
}

/**
  Converts a char value to it's base32 representation, where 0 = a,
  1 = b, ... , 30 = y, 31 = z.

  The function expects that the input character is within allowed range.

  @param char_input The value to convert.

  @return the ASCII equivalent.
*/
char Item_func_geohash::char_to_base32(char char_input)
{
  DBUG_ASSERT(char_input <= 31);

  if (char_input < 10)
    return char_input + '0';
  else if (char_input < 17)
    return char_input + ('b' - 10);
  else if (char_input < 19)
    return char_input + ('b' - 10 + 1);
  else if (char_input < 21)
    return char_input + ('b' - 10 + 2);
  else
    return char_input + ('b' - 10 + 3);
}


bool Item_func_latlongfromgeohash::resolve_type(THD *thd)
{
  if (Item_real_func::resolve_type(thd))
    return true;
  unsigned_flag= false;
  return false;
}


bool Item_func_latlongfromgeohash::fix_fields(THD *thd, Item **ref)
{
  if (Item_real_func::fix_fields(thd, ref))
    return true;

  maybe_null= args[0]->maybe_null;

  if (!check_geohash_argument_valid_type(args[0]))
  {
    my_error(ER_INCORRECT_TYPE, MYF(0), "geohash", func_name());
    return true;
  }

  return false;
}


/**
  Checks if geohash arguments is of valid type

  We must enforce that input actually is text/char, since
  SELECT LongFromGeohash(0123) would give different (and wrong) result,
  as opposed to SELECT LongFromGeohash("0123").

  @param item Item to validate.

  @return false if validation failed. true if item is a valid type.
*/
bool
Item_func_latlongfromgeohash::check_geohash_argument_valid_type(Item *item)
{
  if (is_item_null(item))
    return true;

  /*
    If charset is not binary and field_type() is BLOB,
    we have a TEXT column (which is allowed).
  */
  bool is_binary_charset= (item->collation.collation == &my_charset_bin);
  bool is_parameter_marker= (item->type() == PARAM_ITEM);

  switch (item->field_type())
  {
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    return (!is_binary_charset || is_parameter_marker);
  default:
    return false;
  }
}


/**
  Decodes a geohash string into longitude and latitude.

  The results are rounded,  based on the length of input geohash. The function
  will stop evaluating when the error range, or "accuracy", has become 0.0 for
  both latitude and longitude since no more changes can happen after this.

  @param geohash The geohash to decode.
  @param upper_latitude Upper limit of returned latitude (normally 90.0).
  @param lower_latitude Lower limit of returned latitude (normally -90.0).
  @param upper_longitude Upper limit of returned longitude (normally 180.0).
  @param lower_longitude Lower limit of returned longitude (normally -180.0).
  @param[out] result_latitude Calculated latitude.
  @param[out] result_longitude Calculated longitude.

  @return false on success, true on failure (invalid geohash string).
*/
bool
Item_func_latlongfromgeohash::decode_geohash(String *geohash,
                                             double upper_latitude,
                                             double lower_latitude,
                                             double upper_longitude,
                                             double lower_longitude,
                                             double *result_latitude,
                                             double *result_longitude)
{
  double latitude_accuracy= (upper_latitude - lower_latitude) / 2.0;
  double longitude_accuracy= (upper_longitude - lower_longitude) / 2.0;

  double latitude_value= (upper_latitude + lower_latitude) / 2.0;
  double longitude_value= (upper_longitude + lower_longitude) / 2.0;

  uint number_of_bits_used= 0;
  uint input_length= geohash->length();

  for (uint i= 0;
       i < input_length && latitude_accuracy > 0.0 && longitude_accuracy > 0.0;
       i++)
  {
    char input_character= my_tolower(&my_charset_latin1, (*geohash)[i]);

    /*
     The following part will convert from character value to a
     contiguous value from 0 to 31, where "0" = 0, "1" = 1 ... "z" = 31.
     It will also detect characters that aren't allowed.
    */
    int converted_character;
    if (input_character >= '0' && input_character <= '9')
    {
      converted_character= input_character - '0';
    }
    else if (input_character >= 'b' && input_character <= 'z' &&
             input_character != 'i' &&
             input_character != 'l' &&
             input_character != 'o')
    {
      if (input_character > 'o')
        converted_character= input_character - ('b' - 10 + 3);
      else if (input_character > 'l')
        converted_character= input_character - ('b' - 10 + 2);
      else if (input_character > 'i')
        converted_character= input_character - ('b' - 10 + 1);
      else
        converted_character= input_character - ('b' - 10);
    }
    else
    {
      return true;
    }

    DBUG_ASSERT(converted_character >= 0 && converted_character <= 31);

    /*
     This loop decodes 5 bits of data. Every even bit (counting from 0) is
     used for longitude value, and odd bits are used for latitude value.
    */
    for (int bit_number= 4; bit_number >= 0; bit_number-= 1)
    {
      if (number_of_bits_used % 2 == 0)
      {
        longitude_accuracy/= 2.0;

        if (converted_character & (1 << bit_number))
          longitude_value+= longitude_accuracy;
        else
          longitude_value-= longitude_accuracy;
      }
      else
      {
        latitude_accuracy/= 2.0;

        if (converted_character & (1 << bit_number))
          latitude_value+= latitude_accuracy;
        else
          latitude_value-= latitude_accuracy;
      }

      number_of_bits_used++;

      DBUG_ASSERT(latitude_value >= lower_latitude &&
                  latitude_value <= upper_latitude &&
                  longitude_value >= lower_longitude &&
                  longitude_value <= upper_longitude);
    }
  }

  *result_latitude= round_latlongitude(latitude_value,
                                       latitude_accuracy * 2.0,
                                       latitude_value - latitude_accuracy,
                                       latitude_value + latitude_accuracy);
  *result_longitude= round_latlongitude(longitude_value,
                                        longitude_accuracy * 2.0,
                                        longitude_value - longitude_accuracy,
                                        longitude_value + longitude_accuracy);

  /*
    Ensure that the rounded results are not ouside of the valid range. As
    written in the specification:

      Final rounding should be done carefully in a way that
                min <= round(value) <= max
  */
  DBUG_ASSERT(latitude_value - latitude_accuracy <= *result_latitude);
  DBUG_ASSERT(*result_latitude <= latitude_value + latitude_accuracy);

  DBUG_ASSERT(longitude_value - longitude_accuracy <= *result_longitude);
  DBUG_ASSERT(*result_longitude <= longitude_value + longitude_accuracy);

  return false;
}


/**
  Rounds a latitude or longitude value.

  This will round a latitude or longitude value, based on error_range.
  The error_range is the difference between upper and lower lat/longitude
  (e.g upper value of 45.0 and a lower value of 22.5, gives an error range of
  22.5).

  The returned result will always be in the range [lower_limit, upper_limit]

  @param latlongitude The latitude or longitude to round.
  @param error_range The total error range of the calculated laglongitude.
  @param lower_limit Lower limit of the returned result.
  @param upper_limit Upper limit of the returned result.

  @return A rounded latitude or longitude.
*/
double Item_func_latlongfromgeohash::round_latlongitude(double latlongitude,
                                                        double error_range,
                                                        double lower_limit,
                                                        double upper_limit)
{
  // Ensure that we don't start with an impossible case to solve.
  DBUG_ASSERT(lower_limit <= latlongitude);
  DBUG_ASSERT(upper_limit >= latlongitude);

  if (error_range == 0.0)
  {
    return latlongitude;
  }
  else
  {
    uint number_of_decimals= 0;
    while (error_range <= 0.1 && number_of_decimals <= DBL_DIG)
    {
      number_of_decimals++;
      error_range*= 10.0;
    }

    double return_value;
    do
    {
      return_value= my_double_round(latlongitude, number_of_decimals, false,
                                    false);
      number_of_decimals++;
    } while ((lower_limit > return_value || return_value > upper_limit) &&
             number_of_decimals <= DBL_DIG);

    /*
      We may in some cases still be outside of the allowed range. If this is the
      case, return the input value (which we know for sure to be within the
      allowed range).
    */
    if (lower_limit > return_value || return_value > upper_limit)
      return_value= latlongitude;

    // Avoid printing signed zero.
    return return_value + 0.0;
  }
}


/**
  Decodes a geohash into longitude if start_on_even_bit == true, or latitude if
  start_on_even_bit == false. The output will be rounded based on the length
  of the geohash.
*/
double Item_func_latlongfromgeohash::val_real()
{
  DBUG_ASSERT(fixed == TRUE);

  String buf;
  String *input_value= args[0]->val_str_ascii(&buf);
  DBUG_ASSERT(input_value != NULL || args[0]->null_value);

  if ((null_value= (input_value == NULL || args[0]->null_value)))
    return 0.0;

  if (input_value->length() == 0)
  {
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "geohash",
             input_value->c_ptr_safe(), func_name());
    return error_real();
  }

  double latitude= 0.0;
  double longitude= 0.0;
  if (decode_geohash(input_value, upper_latitude, lower_latitude,
                     upper_longitude, lower_longitude, &latitude, &longitude))
  {
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "geohash",
             input_value->c_ptr_safe(), func_name());
    return error_real();
  }

  // Return longitude if start_on_even_bit == true. Otherwise, return latitude.
  if (start_on_even_bit)
    return longitude;
  return latitude;
}


String *Item_func_as_wkt::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0;

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  str->length(0);
  if ((null_value= geom->as_wkt(str)))
    return 0;

  return str;
}


bool Item_func_as_wkt::resolve_type(THD *thd)
{
  collation.set(default_charset(), DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length=MAX_BLOB_WIDTH;
  maybe_null= true;
  return false;
}


String *Item_func_as_wkb::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0;

  if (!(Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  str->copy(swkb->ptr() + SRID_SIZE, swkb->length() - SRID_SIZE,
	    &my_charset_bin);
  return str;
}


String *Item_func_geometry_type::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *swkb= args[0]->val_str(str);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0;

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }
  /* String will not move */
  str->copy(geom->get_class_info()->m_name.str,
	    geom->get_class_info()->m_name.length,
	    &my_charset_latin1);
  return str;
}


String *Item_func_validate::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return error_str();
  if (!(geom= Geometry::construct(&buffer, swkb)))
    return error_str();

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      my_error(ER_SRS_NOT_CARTESIAN_UNDEFINED, MYF(0), func_name(),
               geom->get_srid());
      return error_str();
    }
    if (!srs->is_cartesian())
    {
      my_error(ER_SRS_NOT_CARTESIAN, MYF(0), func_name(), geom->get_srid());
      return error_str();
    }
  }

  int isvalid= 0;

  try
  {
    isvalid= check_geometry_valid(geom);
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("ST_Validate");
  }

  return isvalid ? swkb : error_str();
}


Field::geometry_type Item_func_make_envelope::get_geometry_type() const
{
  return Field::GEOM_POLYGON;
}


String *Item_func_make_envelope::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val1, arg_val2;
  String *pt1= args[0]->val_str(&arg_val1);
  String *pt2= args[1]->val_str(&arg_val2);
  Geometry_buffer buffer1, buffer2;
  Geometry *geom1= NULL, *geom2= NULL;
  uint32 srid;

  if ((null_value= (!pt1 || !pt2 ||
                    args[0]->null_value || args[1]->null_value)))
    return error_str();
  if ((null_value= (!(geom1= Geometry::construct(&buffer1, pt1)) ||
                    !(geom2= Geometry::construct(&buffer2, pt2)))))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom1->get_srid() != geom2->get_srid())
  {
    my_error(ER_GIS_DIFFERENT_SRIDS, MYF(0), func_name(),
             geom1->get_srid(), geom2->get_srid());
    return error_str();
  }

  if (geom1->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom1->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      my_error(ER_SRS_NOT_CARTESIAN_UNDEFINED, MYF(0), func_name(), geom1->get_srid());
      return error_str();
    }
    if (!srs->is_cartesian())
    {
      my_error(ER_SRS_NOT_CARTESIAN, MYF(0), func_name(), geom1->get_srid());
      return error_str();
    }
  }

  if (geom1->get_type() != Geometry::wkb_point ||
      geom2->get_type() != Geometry::wkb_point)
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }

  if (geom1->get_srid() != geom2->get_srid())
  {
    my_error(ER_GIS_DIFFERENT_SRIDS, MYF(0), func_name(),
             geom1->get_srid(), geom2->get_srid());
    return error_str();
  }

  Gis_point *gpt1= static_cast<Gis_point *>(geom1);
  Gis_point *gpt2= static_cast<Gis_point *>(geom2);
  double x1= gpt1->get<0>(), y1= gpt1->get<1>();
  double x2= gpt2->get<0>(), y2= gpt2->get<1>();

  if (!std::isfinite(x1) || !std::isfinite(x2) ||
      !std::isfinite(y1) || !std::isfinite(y2))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  MBR mbr;

  if (x1 < x2)
  {
    mbr.xmin= x1;
    mbr.xmax= x2;
  }
  else
  {
    mbr.xmin= x2;
    mbr.xmax= x1;
  }

  if (y1 < y2)
  {
    mbr.ymin= y1;
    mbr.ymax= y2;
  }
  else
  {
    mbr.ymin= y2;
    mbr.ymax= y1;
  }

  int dim= mbr.dimension();
  DBUG_ASSERT(dim >= 0);

  /*
    Use default invalid SRID because we are computing the envelope on an
    "abstract plain", that is, we are not aware of any detailed information
    of the coordinate system, we simply suppose the points given to us
    are in the abstract cartesian coordinate system, so we always use the
    point with minimum coordinates of all dimensions and the point with maximum
    coordinates of all dimensions and combine the two groups of coordinate
    values  to get 2^N points for the N dimension points.
   */
  srid= 0;
  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(GEOM_HEADER_SIZE + 4 + 4 + 5 * POINT_DATA_SIZE, 128))
    return error_str();

  str->q_append(srid);
  str->q_append(static_cast<char>(Geometry::wkb_ndr));

  if (dim == 0)
  {
    str->q_append(static_cast<uint32>(Geometry::wkb_point));
    str->q_append(mbr.xmin);
    str->q_append(mbr.ymin);
  }
  else if (dim == 1)
  {

    str->q_append(static_cast<uint32>(Geometry::wkb_linestring));
    str->q_append(static_cast<uint32>(2));
    str->q_append(mbr.xmin);
    str->q_append(mbr.ymin);
    str->q_append(mbr.xmax);
    str->q_append(mbr.ymax);
  }
  else
  {
    DBUG_ASSERT(dim == 2);
    str->q_append(static_cast<uint32>(Geometry::wkb_polygon));
    str->q_append(static_cast<uint32>(1));
    str->q_append(static_cast<uint32>(5));
    str->q_append(mbr.xmin);
    str->q_append(mbr.ymin);
    str->q_append(mbr.xmax);
    str->q_append(mbr.ymin);
    str->q_append(mbr.xmax);
    str->q_append(mbr.ymax);
    str->q_append(mbr.xmin);
    str->q_append(mbr.ymax);
    str->q_append(mbr.xmin);
    str->q_append(mbr.ymin);
  }

  return str;
}


Field::geometry_type Item_func_envelope::get_geometry_type() const
{
  return Field::GEOM_GEOMETRY;
}


String *Item_func_envelope::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value= (!swkb || args[0]->null_value)))
  {
    DBUG_ASSERT(!swkb && args[0]->null_value);
    return NULL;
  }

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  srid= uint4korr(swkb->ptr());
  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(SRID_SIZE, 512))
    return error_str();
  str->q_append(srid);
  if ((null_value= geom->envelope(str)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  return str;
}


Field::geometry_type Item_func_centroid::get_geometry_type() const
{
  return Field::GEOM_POINT;
}


String *Item_func_centroid::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return NULL;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  str->length(0);
  str->set_charset(&my_charset_bin);

  if (geom->get_geotype() != Geometry::wkb_geometrycollection &&
      geom->normalize_ring_order() == NULL)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  null_value= bg_centroid<bgcs::cartesian>(geom, str);
  if (null_value)
    return error_str();
  return str;
}


/**
   Accumulate a geometry's all vertex points into a multipoint.
   It implements the WKB_scanner_event_handler interface so as to be registered
   into wkb_scanner and be notified of WKB data events.
 */
class Point_accumulator : public WKB_scanner_event_handler
{
  Gis_multi_point *m_mpts;
  const void *pt_start;
public:
  explicit Point_accumulator(Gis_multi_point *mpts)
    :m_mpts(mpts), pt_start(NULL)
  {
  }

  virtual void on_wkb_start(Geometry::wkbByteOrder bo,
                            Geometry::wkbType geotype,
                            const void *wkb, uint32 len, bool has_hdr)
  {
    if (geotype == Geometry::wkb_point)
    {
      Gis_point pt(wkb, POINT_DATA_SIZE,
                   Geometry::Flags_t(Geometry::wkb_point, len),
                   m_mpts->get_srid());
      m_mpts->push_back(pt);
      pt_start= wkb;
    }
  }


  virtual void on_wkb_end(const void *wkb)
  {
    if (pt_start)
      DBUG_ASSERT(static_cast<const char *>(pt_start) + POINT_DATA_SIZE == wkb);

    pt_start= NULL;
  }
};


/**
  Retrieve from a geometry collection geometries of the same base type into
  a multi-xxx geometry object. For example, group all points and multipoints
  into a single multipoint object, where the base type is point.

  @tparam Base_type the base type to group.
*/
template <typename Base_type>
class Geometry_grouper : public WKB_scanner_event_handler
{
  std::vector<Geometry::wkbType> m_types;
  std::vector<const void *> m_ptrs;

  typedef Gis_wkb_vector<Base_type> Group_type;
  Group_type *m_group;
  Gis_geometry_collection *m_collection;
  String *m_gcbuf;
  Geometry::wkbType m_target_type;

public:
  explicit Geometry_grouper(Group_type *out)
    :m_group(out), m_collection(NULL), m_gcbuf(NULL)
  {
    switch (out->get_type())
    {
    case Geometry::wkb_multipoint:
      m_target_type= Geometry::wkb_point;
      break;
    case Geometry::wkb_multilinestring:
      m_target_type= Geometry::wkb_linestring;
      break;
    case Geometry::wkb_multipolygon:
      m_target_type= Geometry::wkb_polygon;
      break;
    default:
      DBUG_ASSERT(false);
      break;
    }
  }

  /*
    Group polygons and multipolygons into a geometry collection.
  */
  Geometry_grouper(Gis_geometry_collection *out, String *gcbuf)
    :m_group(NULL), m_collection(out), m_gcbuf(gcbuf)
  {
    m_target_type= Geometry::wkb_polygon;
    DBUG_ASSERT(out != NULL && gcbuf != NULL);
  }


  virtual void on_wkb_start(Geometry::wkbByteOrder bo,
                            Geometry::wkbType geotype,
                            const void *wkb, uint32 len, bool has_hdr)
  {
    m_types.push_back(geotype);
    m_ptrs.push_back(wkb);

    if (m_types.size() == 1)
      DBUG_ASSERT(geotype == Geometry::wkb_geometrycollection);
  }


  virtual void on_wkb_end(const void *wkb_end)
  {
    Geometry::wkbType geotype= m_types.back();
    m_types.pop_back();

    const void *wkb_start= m_ptrs.back();
    m_ptrs.pop_back();

    if (geotype != m_target_type || m_types.size() == 0)
      return;

    Geometry::wkbType ptype= m_types.back();
    size_t len= static_cast<const char *>(wkb_end) -
      static_cast<const char *>(wkb_start);

    /*
      We only group independent geometries, points in linestrings or polygons
      are not independent, nor are linestrings in polygons.
     */
    if (m_target_type == geotype && m_group != NULL &&
        ((m_target_type == Geometry::wkb_point &&
          (ptype == Geometry::wkb_geometrycollection ||
           ptype == Geometry::wkb_multipoint)) ||
         (m_target_type == Geometry::wkb_linestring &&
          (ptype == Geometry::wkb_geometrycollection ||
           ptype == Geometry::wkb_multilinestring)) ||
         (m_target_type == Geometry::wkb_polygon &&
          (ptype == Geometry::wkb_geometrycollection ||
           ptype == Geometry::wkb_multipolygon))))
    {
      Base_type g(wkb_start, len, Geometry::Flags_t(m_target_type, 0), 0);
      m_group->push_back(g);
      DBUG_ASSERT(m_collection == NULL && m_gcbuf == NULL);
    }

    if (m_collection != NULL && (geotype == Geometry::wkb_polygon ||
                                 geotype == Geometry::wkb_multipolygon))
    {
      DBUG_ASSERT(m_group == NULL && m_gcbuf != NULL);
      String str(static_cast<const char *>(wkb_start), len, &my_charset_bin);
      m_collection->append_geometry(m_collection->get_srid(), geotype,
                                    &str, m_gcbuf);
    }
  }
};


/*
  Compute a geometry collection's centroid in demension decreasing order:
  If it has polygons, make them a multipolygon and compute its centroid as the
  result; otherwise compose a multilinestring and compute its centroid as the
  result; otherwise compose a multipoint and compute its centroid as the result.
  @param geom the geometry collection.
  @param[out] respt takes out the centroid point result.
  @param[out] null_value returns whether the result is NULL.
  @return whether got error, true if got error and false if successful.
*/
template <typename Coordsys>
bool geometry_collection_centroid(const Geometry *geom,
                                  typename BG_models<Coordsys>::
                                  Point *respt, my_bool *null_value)
{
  typename BG_models<Coordsys>::Multipolygon mplgn;
  Geometry_grouper<typename BG_models<Coordsys>::Polygon>
    plgn_grouper(&mplgn);

  const char *wkb_start= geom->get_cptr();
  uint32 wkb_len0, wkb_len= geom->get_data_size();
  *null_value= false;

  /*
    The geometries with largest dimension determine the centroid, because
    components of lower dimensions weighs nothing in comparison.
   */
  wkb_len0= wkb_len;
  wkb_scanner(wkb_start, &wkb_len,
              Geometry::wkb_geometrycollection, false, &plgn_grouper);
  if (mplgn.size() > 0)
  {
    if (mplgn.normalize_ring_order() == NULL)
      return true;

    boost::geometry::centroid(mplgn, *respt);
  }
  else
  {
    typename BG_models<Coordsys>::Multilinestring mls;
    wkb_len= wkb_len0;
    Geometry_grouper<typename BG_models<Coordsys>::Linestring>
      ls_grouper(&mls);
    wkb_scanner(wkb_start, &wkb_len,
                Geometry::wkb_geometrycollection, false, &ls_grouper);
    if (mls.size() > 0)
      boost::geometry::centroid(mls, *respt);
    else
    {
      typename BG_models<Coordsys>::Multipoint mpts;
      wkb_len= wkb_len0;
      Geometry_grouper<typename BG_models<Coordsys>::Point>
        pt_grouper(&mpts);
      wkb_scanner(wkb_start, &wkb_len,
                  Geometry::wkb_geometrycollection, false, &pt_grouper);
      if (mpts.size() > 0)
        boost::geometry::centroid(mpts, *respt);
      else
        *null_value= true;
    }
  }

  return false;
}


template <typename Coordsys>
bool Item_func_centroid::bg_centroid(const Geometry *geom, String *ptwkb)
{
  typename BG_models<Coordsys>::Point respt;

  // Release last call's result buffer.
  bg_resbuf_mgr.free_result_buffer();

  try
  {
    switch (geom->get_type())
    {
    case Geometry::wkb_point:
      {
        typename BG_models<Coordsys>::Point
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_multipoint:
      {
        typename BG_models<Coordsys>::Multipoint
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_linestring:
      {
        typename BG_models<Coordsys>::Linestring
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_multilinestring:
      {
        typename BG_models<Coordsys>::Multilinestring
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_polygon:
      {
        typename BG_models<Coordsys>::Polygon
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_multipolygon:
      {
        typename BG_models<Coordsys>::Multipolygon
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::centroid(geo, respt);
      }
      break;
    case Geometry::wkb_geometrycollection:
      if (geometry_collection_centroid<Coordsys>(geom, &respt, &null_value))
      {
        my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
        null_value= true;
      }
      break;
    default:
      DBUG_ASSERT(false);
      break;
    }

    respt.set_srid(geom->get_srid());
    if (!null_value)
      null_value= post_fix_result(&bg_resbuf_mgr, respt, ptwkb);
    if (!null_value)
      bg_resbuf_mgr.set_result_buffer(const_cast<char *>(ptwkb->ptr()));
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("st_centroid");
  }

  return null_value;
}

Field::geometry_type Item_func_convex_hull::get_geometry_type() const
{
  return Field::GEOM_GEOMETRY;
}

String *Item_func_convex_hull::val_str(String *str)
{
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return NULL;

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  DBUG_ASSERT(geom->get_coordsys() == Geometry::cartesian);
  str->set_charset(&my_charset_bin);
  str->length(0);

  if (geom->get_geotype() != Geometry::wkb_geometrycollection &&
      geom->normalize_ring_order() == NULL)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  if (bg_convex_hull<bgcs::cartesian>(geom, str))
    return error_str();

  // By taking over, str owns swkt->ptr and the memory will be released when
  // str points to another buffer in next call of this function
  // (done in post_fix_result), or when str's owner Item_xxx node is destroyed.
  if (geom->get_type() == Geometry::wkb_point)
    str->takeover(*swkb);

  return str;
}


template <typename Coordsys>
bool Item_func_convex_hull::bg_convex_hull(const Geometry *geom,
                                           String *res_hull)
{
  typename BG_models<Coordsys>::Polygon hull;
  typename BG_models<Coordsys>::Linestring line_hull;
  Geometry::wkbType geotype= geom->get_type();

  // Release last call's result buffer.
  bg_resbuf_mgr.free_result_buffer();

  try
  {
    if (geotype == Geometry::wkb_multipoint ||
        geotype == Geometry::wkb_linestring ||
        geotype == Geometry::wkb_multilinestring ||
        geotype == Geometry::wkb_geometrycollection)
    {
      /*
        It's likely that the multilinestring, linestring, geometry collection
        and multipoint have all colinear points so the final hull is a
        linear hull. If so we must get the linear hull otherwise we will get
        an invalid polygon hull.
       */
      typename BG_models<Coordsys>::Multipoint mpts;
      Point_accumulator pt_acc(&mpts);
      const char *wkb_start= geom->get_cptr();
      uint32 wkb_len= geom->get_data_size();
      wkb_scanner(wkb_start, &wkb_len, geotype, false, &pt_acc);
      bool isdone= true;
      if (mpts.size() == 0)
        return (null_value= true);

      // Make mpts unique as required by the is_colinear() algorithm.
      typename BG_models<Coordsys>::Multipoint distinct_pts;
      distinct_pts.resize(mpts.size());
      std::sort(mpts.begin(), mpts.end(), bgpt_lt());
      typename BG_models<Coordsys>::Multipoint::iterator itr=
        std::unique_copy(mpts.begin(), mpts.end(),
                         distinct_pts.begin(), bgpt_eq());
      distinct_pts.resize(itr - distinct_pts.begin());

      if (is_colinear(distinct_pts))
      {
        if (distinct_pts.size() == 1)
        {
          // Here we have to create a brand new point because res_hull will
          // take over its memory, which can't be done to distinct_pts[0].
          typename BG_models<Coordsys>::Point pt_hull= distinct_pts[0];
          pt_hull.set_srid(geom->get_srid());
          null_value= post_fix_result(&bg_resbuf_mgr, pt_hull, res_hull);
        }
        else
        {
          boost::geometry::convex_hull(distinct_pts, line_hull);
          line_hull.set_srid(geom->get_srid());
          // The linestring consists of 4 or more points, but only the
          // first two contain real data, so we need to trim it down.
          line_hull.resize(2);
          null_value= post_fix_result(&bg_resbuf_mgr, line_hull, res_hull);
        }
      }
      else if (geotype == Geometry::wkb_geometrycollection)
      {
        boost::geometry::convex_hull(mpts, hull);
        hull.set_srid(geom->get_srid());
        null_value= post_fix_result(&bg_resbuf_mgr, hull, res_hull);
      }
      else
        isdone= false;

      if (isdone)
      {
        if (!null_value)
          bg_resbuf_mgr.set_result_buffer(const_cast<char *>(res_hull->ptr()));
        return null_value;
      }
    }

    /*
      From here on we don't have to consider linear hulls, it's impossible.

      In theory we can use above multipoint to get convex hull for all 7 types
      of geometries, however we'd better use BG standard logic for each type,
      a tricky example would be: imagine an invalid polygon whose inner ring is
      completely contains its outer ring inside, BG might return the outer ring
      but if using the multipoint to get convexhull, we would get the
      inner ring as result instead.
    */
    switch (geotype)
    {
    case Geometry::wkb_point:
      {
        /*
          A point's convex hull is the point itself, directly use the point's
          WKB buffer, set its header info correctly.
        */
        DBUG_ASSERT(geom->get_ownmem() == false &&
                    geom->has_geom_header_space());
        char *p= geom->get_cptr() - GEOM_HEADER_SIZE;
        write_geometry_header(p, geom->get_srid(), geom->get_geotype());
        return false;
      }
      break;
    case Geometry::wkb_multipoint:
      {
        typename BG_models<Coordsys>::Multipoint
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::convex_hull(geo, hull);
      }
      break;
    case Geometry::wkb_linestring:
      {
        typename BG_models<Coordsys>::Linestring
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::convex_hull(geo, hull);
      }
      break;
    case Geometry::wkb_multilinestring:
      {
        typename BG_models<Coordsys>::Multilinestring
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::convex_hull(geo, hull);
      }
      break;
    case Geometry::wkb_polygon:
      {
        typename BG_models<Coordsys>::Polygon
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::convex_hull(geo, hull);
      }
      break;
    case Geometry::wkb_multipolygon:
      {
        typename BG_models<Coordsys>::Multipolygon
          geo(geom->get_data_ptr(), geom->get_data_size(),
              geom->get_flags(), geom->get_srid());
        boost::geometry::convex_hull(geo, hull);
      }
      break;
    case Geometry::wkb_geometrycollection:
      // Handled above.
      DBUG_ASSERT(false);
      break;
    default:
      break;
    }

    hull.set_srid(geom->get_srid());
    null_value= post_fix_result(&bg_resbuf_mgr, hull, res_hull);
    if (!null_value)
      bg_resbuf_mgr.set_result_buffer(const_cast<char *>(res_hull->ptr()));
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("st_convexhull");
  }

  return null_value;
}


String *Item_func_simplify::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *swkb= args[0]->val_str(&arg_val);
  double max_dist= args[1]->val_real();
  Geometry_buffer buffer;
  Geometry *geom= NULL;

  // Release last call's result buffer.
  bg_resbuf_mgr.free_result_buffer();

  if ((null_value= (!swkb || args[0]->null_value || args[1]->null_value)))
    return error_str();
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_str(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  if (max_dist <= 0 || boost::math::isnan(max_dist))
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }

  Geometry::wkbType gtype= geom->get_type();

  try
  {
    if (gtype == Geometry::wkb_geometrycollection)
    {
      BG_geometry_collection bggc;
      bggc.fill(geom);
      Gis_geometry_collection gc(geom->get_srid(),
                                 Geometry::wkb_invalid_type, NULL, str);
      for (BG_geometry_collection::Geometry_list::iterator
           i= bggc.get_geometries().begin();
           i != bggc.get_geometries().end(); ++i)
      {
        String gbuf;
        if ((null_value= simplify_basic<bgcs::cartesian>
             (*i, max_dist, &gbuf, &gc, str)))
          return error_str();
      }
    }
    else
    {
      if ((null_value= simplify_basic<bgcs::cartesian>(geom, max_dist, str)))
        return error_str();
      else
        bg_resbuf_mgr.set_result_buffer(const_cast<char *>(str->ptr()));
    }
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("ST_Simplify");
  }

  return str;
}


template <typename Coordsys>
int Item_func_simplify::
simplify_basic(Geometry *geom, double max_dist, String *str,
               Gis_geometry_collection *gc, String *gcbuf)
{
  DBUG_ASSERT((gc == NULL && gcbuf == NULL) || (gc != NULL && gcbuf != NULL));
  Geometry::wkbType geotype= geom->get_type();

  switch (geotype)
  {
  case Geometry::wkb_point:
    {
      typename BG_models<Coordsys>::Point
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_multipoint:
    {
      typename BG_models<Coordsys>::Multipoint
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_linestring:
    {
      typename BG_models<Coordsys>::Linestring
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_multilinestring:
    {
      typename BG_models<Coordsys>::Multilinestring
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_polygon:
    {
      typename BG_models<Coordsys>::Polygon
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_multipolygon:
    {
      typename BG_models<Coordsys>::Multipolygon
        geo(geom->get_data_ptr(), geom->get_data_size(),
            geom->get_flags(), geom->get_srid()), out;
      boost::geometry::simplify(geo, out, max_dist);
      if ((null_value= post_fix_result(&bg_resbuf_mgr, out, str)))
        return null_value;
      if (gc && (null_value= gc->append_geometry(&out, gcbuf)))
        return null_value;
    }
    break;
  case Geometry::wkb_geometrycollection:
  default:
    DBUG_ASSERT(false);
    break;
  }

  return 0;
}


/*
  Spatial decomposition functions
*/

String *Item_func_spatial_decomp::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value= (!swkb || args[0]->null_value)))
    return NULL;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  srid= uint4korr(swkb->ptr());
  str->set_charset(&my_charset_bin);
  if (str->reserve(SRID_SIZE, 512))
    goto err;
  str->length(0);
  str->q_append(srid);
  switch (decomp_func) {
    case SP_STARTPOINT:
      if (geom->start_point(str))
        goto err;
      break;

    case SP_ENDPOINT:
      if (geom->end_point(str))
        goto err;
      break;

    case SP_EXTERIORRING:
      if (geom->exterior_ring(str))
        goto err;
      break;

    default:
      goto err;
  }
  return str;

err:
  null_value= 1;
  return 0;
}


String *Item_func_spatial_decomp_n::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_val;
  String *swkb= args[0]->val_str(&arg_val);
  long n= (long) args[1]->val_int();
  Geometry_buffer buffer;
  Geometry *geom= NULL;
  uint32 srid;

  if ((null_value= (!swkb || args[0]->null_value || args[1]->null_value)))
    return NULL;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  str->set_charset(&my_charset_bin);
  if (str->reserve(SRID_SIZE, 512))
    goto err;
  srid= uint4korr(swkb->ptr());
  str->length(0);
  str->q_append(srid);
  switch (decomp_func_n)
  {
    case SP_POINTN:
      if (geom->point_n(n,str))
        goto err;
      break;

    case SP_GEOMETRYN:
      if (geom->geometry_n(n,str))
        goto err;
      break;

    case SP_INTERIORRINGN:
      if (geom->interior_ring_n(n,str))
        goto err;
      break;

    default:
      goto err;
  }
  return str;

err:
  null_value=1;
  return 0;
}


/*
  Functions to concatenate various spatial objects
*/


/*
*  Concatenate doubles into Point
*/


Field::geometry_type Item_func_point::get_geometry_type() const
{
  return Field::GEOM_POINT;
}


String *Item_func_point::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);

  /*
    The coordinates of a point can't be another geometry, but other types
    are allowed as before.
  */
  if ((null_value= (args[0]->field_type() == MYSQL_TYPE_GEOMETRY ||
                    args[1]->field_type() == MYSQL_TYPE_GEOMETRY)))
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }

  double x= args[0]->val_real();
  double y= args[1]->val_real();
  uint32 srid= 0;

  if ((null_value= (args[0]->null_value ||
                    args[1]->null_value ||
                    str->mem_realloc(4/*SRID*/ + 1 + 4 + SIZEOF_STORED_DOUBLE * 2))))
    return 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  str->q_append(srid);
  str->q_append((char)Geometry::wkb_ndr);
  str->q_append((uint32)Geometry::wkb_point);
  str->q_append(x);
  str->q_append(y);
  return str;
}


/// This will check if arguments passed (geohash and SRID) are of valid types.
bool Item_func_pointfromgeohash::fix_fields(THD *thd, Item **ref)
{
  if (Item_geometry_func::fix_fields(thd, ref))
    return true;

  maybe_null= (args[0]->maybe_null || args[1]->maybe_null);

  // Check for valid type in geohash argument.
  if (!Item_func_latlongfromgeohash::check_geohash_argument_valid_type(args[0]))
  {
    my_error(ER_INCORRECT_TYPE, MYF(0), "geohash", func_name());
    return true;
  }

  /*
    Check for valid type in SRID argument.

    We will allow all integer types, and strings since some connectors will
    covert integers to strings. Binary data is not allowed.

    PARAM_ITEM and INT_ITEM checks are to allow prepared statements and usage of
    user-defined variables respectively.
  */
  if (is_item_null(args[1]))
    return false;

  if (args[1]->collation.collation == &my_charset_bin &&
      args[1]->type() != PARAM_ITEM && args[1]->type() != INT_ITEM)
  {
    my_error(ER_INCORRECT_TYPE, MYF(0), "SRID", func_name());
    return true;
  }

  switch (args[1]->field_type())
  {
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_TINY:
    break;
  default:
    my_error(ER_INCORRECT_TYPE, MYF(0), "SRID", func_name());
    return true;
  }
  return false;
}


String *Item_func_pointfromgeohash::val_str(String *str)
{
  DBUG_ASSERT(fixed == TRUE);

  String argument_value;
  String *geohash= args[0]->val_str_ascii(&argument_value);
  Geometry::srid_t srid= 0;

  if (validate_srid_arg(args[1], &srid, &null_value, func_name()))
    return error_str();

  // Return null if one or more of the input arguments is null.
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return NULL;

  if (str->mem_realloc(GEOM_HEADER_SIZE + POINT_DATA_SIZE))
    return make_empty_result();

  if (geohash->length() == 0)
  {
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "geohash", geohash->c_ptr_safe(),
             func_name());
    return error_str();
  }

  double latitude= 0.0;
  double longitude= 0.0;
  if (Item_func_latlongfromgeohash::decode_geohash(geohash, upper_latitude,
                                                   lower_latitude,
                                                   upper_longitude,
                                                   lower_longitude, &latitude,
                                                   &longitude))
  {
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "geohash", geohash->c_ptr_safe(),
             func_name());
    return error_str();
  }

  str->set_charset(&my_charset_bin);
  str->length(0);
  write_geometry_header(str, srid, Geometry::wkb_point);
  str->q_append(longitude);
  str->q_append(latitude);
  return str;
}


const char *Item_func_spatial_collection::func_name() const
{
  const char *str= NULL;

  switch (coll_type)
  {
  case Geometry::wkb_multipoint:
    str= "multipoint";
    break;
  case Geometry::wkb_multilinestring:
    str= "multilinestring";
    break;
  case Geometry::wkb_multipolygon:
    str= "multipolygon";
    break;
  case Geometry::wkb_linestring:
    str= "linestring";
    break;
  case Geometry::wkb_polygon:
    str= "polygon";
    break;
  case Geometry::wkb_geometrycollection:
    str= "geometrycollection";
    break;
  default:
    DBUG_ASSERT(false);
    break;
  }

  return str;
}


/**
  Concatenates various items into various collections
  with checkings for valid wkb type of items.
  For example, multipoint can be a collection of points only.
  coll_type contains wkb type of target collection.
  item_type contains a valid wkb type of items.
  In the case when coll_type is wkbGeometryCollection,
  we do not check wkb type of items, any is valid.
*/

String *Item_func_spatial_collection::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String arg_value;
  uint i;
  uint32 srid= 0;

  str->set_charset(&my_charset_bin);
  str->length(0);
  if (str->reserve(4/*SRID*/ + 1 + 4 + 4, 512))
    goto err;

  str->q_append(srid);
  str->q_append((char) Geometry::wkb_ndr);
  str->q_append((uint32) coll_type);
  str->q_append((uint32) arg_count);

  // We can construct an empty geometry by calling GeometryCollection().
  if (arg_count == 0)
    return str;

  for (i= 0; i < arg_count; ++i)
  {
    String *res= args[i]->val_str(&arg_value);
    size_t len;

    if (args[i]->null_value || ((len= res->length()) < WKB_HEADER_SIZE))
      goto err;

    if (coll_type == Geometry::wkb_geometrycollection)
    {
      /*
	In the case of GeometryCollection we don't need any checkings
	for item types, so just copy them into target collection
      */
      if (str->append(res->ptr() + 4/*SRID*/, len - 4/*SRID*/, (uint32) 512))
        goto err;
    }
    else
    {
      enum Geometry::wkbType wkb_type;
      const uint data_offset= 4/*SRID*/ + 1;
      if (res->length() < data_offset + sizeof(uint32))
        goto err;
      const char *data= res->ptr() + data_offset;

      /*
	In the case of named collection we must check that items
	are of specific type, let's do this checking now
      */

      wkb_type= get_wkb_geotype(data);
      data+= 4;
      len-= 5 + 4/*SRID*/;
      if (wkb_type != item_type)
      {
        my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
        goto err;
      }

      switch (coll_type) {
      case Geometry::wkb_multipoint:
      case Geometry::wkb_multilinestring:
      case Geometry::wkb_multipolygon:
	if (len < WKB_HEADER_SIZE ||
	    str->append(data-WKB_HEADER_SIZE, len+WKB_HEADER_SIZE, 512))
	  goto err;
	break;

      case Geometry::wkb_linestring:
	if (len < POINT_DATA_SIZE || str->append(data, POINT_DATA_SIZE, 512))
	  goto err;
	break;
      case Geometry::wkb_polygon:
      {
	uint32 n_points;
	double x1, y1, x2, y2;
	const char *org_data= data;

	if (len < 4)
	  goto err;

	n_points= uint4korr(data);
	data+= 4;

        // A ring must have at least 4 points.
        if (n_points < 4 || len != 4 + n_points * POINT_DATA_SIZE)
        {
          my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
          return error_str();
        }

	float8get(&x1, data);
	data+= SIZEOF_STORED_DOUBLE;
	float8get(&y1, data);
	data+= SIZEOF_STORED_DOUBLE;

	data+= (n_points - 2) * POINT_DATA_SIZE;

	float8get(&x2, data);
	float8get(&y2, data + SIZEOF_STORED_DOUBLE);

        // A ring must be closed.
        if ((x1 != x2) || (y1 != y2))
        {
          my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
          return error_str();
        }

	if (str->append(org_data, len, 512))
	  goto err;
      }
      break;

      default:
	goto err;
      }
    }
  }
  if (str->length() > current_thd->variables.max_allowed_packet)
  {
    push_warning_printf(current_thd, Sql_condition::SL_WARNING,
			ER_WARN_ALLOWED_PACKET_OVERFLOWED,
			ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
			func_name(), current_thd->variables.max_allowed_packet);
    goto err;
  }

  if (coll_type == Geometry::wkb_linestring && arg_count < 2)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  /*
    The construct() call parses the string to make sure it's a valid
    WKB byte string instead of some arbitrary trash bytes. Above code assumes
    so and doesn't further completely validate the string's content.

    There are several goto statements above so we have to construct the
    geom_buff object in a scope, this is more of C++ style than defining it at
    start of this function.
  */
  {
    Geometry_buffer geom_buff;
    if (Geometry::construct(&geom_buff, str) == NULL)
    {
      my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
      return error_str();
    }
  }

  null_value= 0;
  return str;

err:
  null_value= 1;
  return 0;
}


BG_geometry_collection::BG_geometry_collection()
  :comp_no_overlapped(false), m_srid(0), m_num_isolated(0),
   m_geobufs(key_memory_Geometry_objects_data),
   m_geosdata(key_memory_Geometry_objects_data)
{}


/**
  Convert this into a Gis_geometry_collection object.
  @param geodata Stores the result object's WKB data.
  @return The Gis_geometry_collection object created from this object.
 */
Gis_geometry_collection *
BG_geometry_collection::as_geometry_collection(String *geodata) const
{
  if (m_geos.size() == 0)
    return empty_collection(geodata, m_srid);

  Gis_geometry_collection *gc= NULL;

  for (Geometry_list::const_iterator i= m_geos.begin();
       i != m_geos.end(); ++i)
  {
    if (gc == NULL)
      gc= new Gis_geometry_collection(*i, geodata);
    else
      gc->append_geometry(*i, geodata);
  }

  return gc;
}


/**
  Store a Geometry object into this collection. If it's a geometry collection,
  flatten it and store its components into this collection, so that no
  component is a geometry collection.
  @param geo The Geometry object to put into this collection. We duplicate
         geo's data rather than directly using it.
  @param break_multi_geom whether break a multipoint or multilinestring or
         multipolygon so as to store its components separately into this object.
  @return true if error occured, false if no error(successful).
 */
bool BG_geometry_collection::store_geometry(const Geometry *geo,
                                            bool break_multi_geom)
{
  Geometry::wkbType geo_type= geo->get_type();

  if ((geo_type == Geometry::wkb_geometrycollection) ||
      (break_multi_geom && (geo_type == Geometry::wkb_multipoint ||
                            geo_type == Geometry::wkb_multipolygon ||
                            geo_type == Geometry::wkb_multilinestring)))
  {
    uint32 ngeom= 0;

    if (geo->num_geometries(&ngeom))
      return true;

    /*
      Get its components and store each of them separately, if a component
      is also a collection, recursively disintegrate and store its
      components in the same way.
     */
    for (uint32 i= 1; i <= ngeom; i++)
    {
      String *pres= m_geosdata.append_object();
      if (pres == NULL || pres->reserve(GEOM_HEADER_SIZE, 512))
        return true;

      pres->q_append(geo->get_srid());
      if (geo->geometry_n(i, pres))
        return true;

      Geometry_buffer *pgeobuf= m_geobufs.append_object();
      if (pgeobuf == NULL)
        return true;
      Geometry *geo2= Geometry::construct(pgeobuf, pres->ptr(),
                                          pres->length());
      if (geo2 == NULL)
      {
        // The geometry data already pass such checks, it's always valid here.
        DBUG_ASSERT(false);
        return true;
      }
      else if (geo2->get_type() == Geometry::wkb_geometrycollection)
      {
        if (store_geometry(geo2, break_multi_geom))
          return true;
      }
      else
      {
        geo2->has_geom_header_space(true);
        m_geos.push_back(geo2);
      }
    }

    /*
      GCs with no-overlapping components can only be returned by
      combine_sub_results, which combines geometries from BG set operations,
      so no nested GCs or other user defined GCs are really set to true here.
    */
    set_comp_no_overlapped(geo->is_components_no_overlapped() || ngeom == 1);
  }
  else if (store(geo) == NULL)
    return true;

  return false;
}


/**
  Store a geometry of GEOMETRY format into this collection.
  @param geo a geometry object whose data of GEOMETRY format is to be duplicated
         and stored into this collection. It's not a geometry collection.
  @return a duplicated Geometry object created from geo.
 */
Geometry *BG_geometry_collection::store(const Geometry *geo)
{
  String *pres= NULL;
  Geometry *geo2= NULL;
  Geometry_buffer *pgeobuf= NULL;
  size_t geosize= geo->get_data_size();

  DBUG_ASSERT(geo->get_type() != Geometry::wkb_geometrycollection);
  pres= m_geosdata.append_object();
  if (pres == NULL || pres->reserve(GEOM_HEADER_SIZE + geosize, 256))
    return NULL;
  write_geometry_header(pres, geo->get_srid(), geo->get_type());
  pres->q_append(geo->get_cptr(), geosize);

  pgeobuf= m_geobufs.append_object();
  if (pgeobuf == NULL)
    return NULL;
  geo2= Geometry::construct(pgeobuf, pres->ptr(), pres->length());
  // The geometry data already pass such checks, it's always valid here.
  DBUG_ASSERT(geo2 != NULL);

  if (geo2 != NULL && geo2->get_type() != Geometry::wkb_geometrycollection)
    m_geos.push_back(geo2);

  return geo2;
}


longlong Item_func_isempty::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Geometry *g= NULL;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0;
  if (!(g= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }

  return (null_value || is_empty_geocollection(g)) ? 1 : 0;
}


longlong Item_func_issimple::val_int()
{
  DBUG_ENTER("Item_func_issimple::val_int");
  DBUG_ASSERT(fixed == 1);

  tmp.length(0);
  String *arg_wkb= args[0]->val_str(&tmp);
  if ((null_value= args[0]->null_value))
  {
    DBUG_ASSERT(maybe_null);
    DBUG_RETURN(0);
  }
  if (arg_wkb == NULL)
  {
    // Invalid geometry.
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    DBUG_RETURN(error_int());
  }

  Geometry_buffer buffer;
  Geometry *arg= Geometry::construct(&buffer, arg_wkb);
  if (arg == NULL)
  {
    // Invalid geometry.
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    DBUG_RETURN(error_int());
  }

  if (arg->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(arg->get_srid(), &srs))
      DBUG_RETURN(error_int()); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          arg->get_srid(),
                          func_name());
    }
  }

  DBUG_RETURN(issimple(arg));
}


/**
  Evaluate if a geometry object is simple according to the OGC definition.

  @param g The geometry to evaluate.
  @return True if the geometry is simple, false otherwise.
*/
bool Item_func_issimple::issimple(Geometry *g)
{
  bool res= false;

  try
  {
    switch (g->get_type())
    {
    case Geometry::wkb_point:
      {
        Gis_point arg(g->get_data_ptr(), g->get_data_size(),
                      g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
      break;
    case Geometry::wkb_linestring:
      {
        Gis_line_string arg(g->get_data_ptr(), g->get_data_size(),
                            g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
    break;
    case Geometry::wkb_polygon:
      {
        const void *arg_wkb= g->normalize_ring_order();
        if (arg_wkb == NULL)
        {
          // Invalid polygon.
          my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
          return error_bool();
        }
        Gis_polygon arg(arg_wkb, g->get_data_size(),
                        g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
      break;
    case Geometry::wkb_multipoint:
      {
        Gis_multi_point arg(g->get_data_ptr(), g->get_data_size(),
                            g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
      break;
    case Geometry::wkb_multilinestring:
      {
        Gis_multi_line_string arg(g->get_data_ptr(), g->get_data_size(),
                                  g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
      break;
    case Geometry::wkb_multipolygon:
      {
        const void *arg_wkb= g->normalize_ring_order();
        if (arg_wkb == NULL)
        {
          // Invalid multipolygon.
          my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
          return error_bool();
        }
        Gis_multi_polygon arg(arg_wkb, g->get_data_size(),
                              g->get_flags(), g->get_srid());
        res= boost::geometry::is_simple(arg);
      }
      break;
    case Geometry::wkb_geometrycollection:
      {
        BG_geometry_collection collection;
        collection.fill(g);

        res= true;
        for (BG_geometry_collection::Geometry_list::iterator i=
               collection.get_geometries().begin();
             i != collection.get_geometries().end();
             ++i)
        {
          res= issimple(*i);
          if (current_thd->is_error())
          {
            res= error_bool();
            break;
          }
          if (!res)
          {
            break;
          }
        }
      }
      break;
    default:
      DBUG_ASSERT(0);
      break;
    }
  }
  catch (...)
  {
    res= error_bool();
    handle_gis_exception(func_name());
  }

  return res;
}


longlong Item_func_isclosed::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Geometry *geom;
  int isclosed= 0;				// In case of error

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0L;

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }

  null_value= geom->is_closed(&isclosed);

  return (longlong) isclosed;
}


/**
  Checks the validity of a geometry collection, which is valid
  if and only if all its components are valid.
 */
class Geomcoll_validity_checker: public WKB_scanner_event_handler
{
  bool m_isvalid;
  Geometry::srid_t m_srid;
  std::stack<Geometry::wkbType> types;
public:
  explicit Geomcoll_validity_checker(Geometry::srid_t srid)
    :m_isvalid(true), m_srid(srid)
  {
  }

  bool is_valid() const { return m_isvalid; }

  virtual void on_wkb_start(Geometry::wkbByteOrder bo,
                            Geometry::wkbType geotype,
                            const void *wkb, uint32 len, bool has_hdr)
  {
    if (!m_isvalid)
      return;
    Geometry::wkbType top= Geometry::wkb_invalid_type;
    if (types.size() > 0)
      top= types.top();
    else
      DBUG_ASSERT(geotype == Geometry::wkb_geometrycollection);

    types.push(geotype);
    // A geometry collection's vaidity is determined by that of its components.
    if (geotype == Geometry::wkb_geometrycollection)
      return;
    // If not owned by a GC(i.e. not a direct component of a GC), it doesn't
    // influence the GC's validity.
    if (top != Geometry::wkb_geometrycollection)
      return;

    DBUG_ASSERT(top != Geometry::wkb_invalid_type && has_hdr);
    DBUG_ASSERT(len > WKB_HEADER_SIZE);

    Geometry_buffer geobuf;
    Geometry *geo= NULL;

    // Provide the WKB header starting address, wkb MUST have a WKB header
    // right before it.
    geo= Geometry::construct(&geobuf,
                             static_cast<const char *>(wkb) - WKB_HEADER_SIZE,
                             len + WKB_HEADER_SIZE, false/* has no srid */);
    if (geo == NULL)
      m_isvalid= false;
    else
    {
      geo->set_srid(m_srid);
      m_isvalid= check_geometry_valid(geo);
    }
  }


  virtual void on_wkb_end(const void *wkb)
  {
    if (types.size() > 0)
      types.pop();
  }
};


/*
  Call Boost Geometry algorithm to check whether a geometry is valid as
  defined by OGC.
 */
static int check_geometry_valid(Geometry *geom)
{
  int ret= 0;

  // Empty geometry collection is always valid. This is shortcut for
  // flat empty GCs.
  if (is_empty_geocollection(geom))
    return 1;

  switch (geom->get_type())
  {
  case Geometry::wkb_point:
  {
    BG_models<bgcs::cartesian>::Point
      bg(geom->get_data_ptr(), geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_linestring:
  {
    BG_models<bgcs::cartesian>::Linestring
      bg(geom->get_data_ptr(), geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_polygon:
  {
    const void *ptr= geom->normalize_ring_order();
    if (ptr == NULL)
    {
      ret= 0;
      break;
    }

    BG_models<bgcs::cartesian>::Polygon
      bg(ptr, geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_multipoint:
  {
    BG_models<bgcs::cartesian>::Multipoint
      bg(geom->get_data_ptr(), geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_multilinestring:
  {
    BG_models<bgcs::cartesian>::Multilinestring
      bg(geom->get_data_ptr(), geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_multipolygon:
  {
    const void *ptr= geom->normalize_ring_order();
    if (ptr == NULL)
    {
      ret= 0;
      break;
    }

    BG_models<bgcs::cartesian>::Multipolygon
      bg(ptr, geom->get_data_size(),
         geom->get_flags(), geom->get_srid());
    ret= bg::is_valid(bg);
    break;
  }
  case Geometry::wkb_geometrycollection:
  {
    uint32 wkb_len= geom->get_data_size();
    Geomcoll_validity_checker validity_checker(geom->get_srid());

    /*
      This case can only be reached when this function isn't recursively
      called (indirectly by itself) but called by other functions, and
      it's the WKB data doesn't have a WKB header before it. Otherwise in
      Geomcoll_validity_checker it's required that the WKB data has a header.
     */
    wkb_scanner(geom->get_cptr(), &wkb_len,
                Geometry::wkb_geometrycollection, false,
                &validity_checker);
    ret= validity_checker.is_valid();
    break;
  }
  default:
    DBUG_ASSERT(false);
    break;
  }

  return ret;
}


longlong Item_func_isvalid::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String tmp;
  String *swkb= args[0]->val_str(&tmp);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0L;

  // It should return false if the argument isn't a valid GEOMETRY string.
  if (!(geom= Geometry::construct(&buffer, swkb)))
    return 0L;

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_int(); // Error has already been flagged.

    if (srs == nullptr)
    {
      my_error(ER_SRS_NOT_CARTESIAN_UNDEFINED, MYF(0), func_name(), geom->get_srid());
      return error_int();
    }
    if (!srs->is_cartesian())
    {
      my_error(ER_SRS_NOT_CARTESIAN, MYF(0), func_name(), geom->get_srid());
      return error_int();
    }
  }

  int ret= 0;
  try
  {
    ret= check_geometry_valid(geom);
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("ST_IsValid");
  }

  return ret;
}


/*
  Numerical functions
*/


longlong Item_func_dimension::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint32 dim= 0;				// In case of error
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }
  null_value= geom->dimension(&dim);
  return (longlong) dim;
}


longlong Item_func_numinteriorring::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint32 num= 0;				// In case of error
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0L;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }
  null_value= geom->num_interior_ring(&num);
  return (longlong) num;
}


longlong Item_func_numgeometries::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint32 num= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0L;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }
  null_value= geom->num_geometries(&num);
  return (longlong) num;
}


longlong Item_func_numpoints::val_int()
{
  DBUG_ASSERT(fixed == 1);
  uint32 num= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return 0L;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }
  null_value= geom->num_points(&num);
  return (longlong) num;
}

String *Item_func_set_x::val_str(String *str)
{
  DBUG_ASSERT(fixed);
  String *swkb= args[0]->val_str(str);
  double x_coordinate= args[1]->val_real();

  if ((null_value= (args[0]->null_value || args[1]->null_value)))
  {
    return nullptr;
  }

  if (!swkb)
  {
    /*
    We've already found out that args[0]->null_value is false.
    Therefore, swkb should never be null.
    */
    DBUG_ASSERT(false);
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (std::isnan(x_coordinate) || std::isinf(x_coordinate))
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), func_name());
    return error_str();
  }

  Geometry_buffer buffer;
  Geometry *geom;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_type() != Geometry::wkb_point)
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "POINT",
             geom->get_class_info()->m_name.str, func_name());
    return error_str();
  }

  str->copy(*swkb);
  float8store(str->c_ptr_safe() + GEOM_HEADER_SIZE, x_coordinate);
  return str;
}

String *Item_func_set_y::val_str(String *str)
{
  DBUG_ASSERT(fixed);
  String *swkb= args[0]->val_str(str);
  double y_coordinate= args[1]->val_real();

  if ((null_value= (args[0]->null_value || args[1]->null_value)))
  {
    return nullptr;
  }

  if (!swkb)
  {
    /*
    We've already found out that args[0]->null_value is false.
    Therefore, swkb should never be null.
    */
    DBUG_ASSERT(false);
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (std::isnan(y_coordinate) || std::isinf(y_coordinate))
  {
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), func_name());
    return error_str();
  }

  Geometry_buffer buffer;
  Geometry *geom;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (geom->get_type() != Geometry::wkb_point)
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "POINT",
             geom->get_class_info()->m_name.str, func_name());
    return error_str();
  }

  str->copy(*swkb);
  float8store(str->c_ptr_safe() + GEOM_HEADER_SIZE + SIZEOF_STORED_DOUBLE,
              y_coordinate);
  return str;
}

double Item_func_get_x::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double res= 0.0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (args[0]->null_value)))
  {
    return 0.0;
  }

  if (!swkb)
  {
    /* 
    We've already found out that args[0]->null_value is false.
    Therefore, swkb should never be null.
    */
    DBUG_ASSERT(false);
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (geom->get_type() != Geometry::wkb_point)
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "POINT",
             geom->get_class_info()->m_name.str, func_name());
    return error_real();
  }
  null_value= geom->get_x(&res);
  return res;
}


double Item_func_get_y::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (args[0]->null_value)))
  {
    return 0.0;
  }

  if (!swkb)
  {
    /*
    We've already found out that args[0]->null_value is false.
    Therefore, swkb should never be null.
    */
    DBUG_ASSERT(false);
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (geom->get_type() != Geometry::wkb_point)
  {
    my_error(ER_UNEXPECTED_GEOMETRY_TYPE, MYF(0), "POINT",
             geom->get_class_info()->m_name.str, func_name());
    return error_real();
  }
  null_value= geom->get_y(&res);
  return res;
}


template <typename Coordsys>
double Item_func_area::bg_area(const Geometry *geom)
{
  double res= 0;

  try
  {
    switch (geom->get_type())
    {
    case Geometry::wkb_point:
    case Geometry::wkb_multipoint:
    case Geometry::wkb_linestring:
    case Geometry::wkb_multilinestring:
      res= 0;
      break;
    case Geometry::wkb_polygon:
      {
        typename BG_models<Coordsys>::Polygon
          plgn(geom->get_data_ptr(), geom->get_data_size(),
               geom->get_flags(), geom->get_srid());

        res= boost::geometry::area(plgn);
      }
      break;
    case Geometry::wkb_multipolygon:
      {
        typename BG_models<Coordsys>::Multipolygon
          mplgn(geom->get_data_ptr(), geom->get_data_size(),
                geom->get_flags(), geom->get_srid());

        res= boost::geometry::area(mplgn);
      }
      break;
    case Geometry::wkb_geometrycollection:
      {
        BG_geometry_collection bggc;

        bggc.fill(geom);

        for (BG_geometry_collection::Geometry_list::iterator
             i= bggc.get_geometries().begin();
             i != bggc.get_geometries().end(); ++i)
        {
          if ((*i)->get_geotype() != Geometry::wkb_geometrycollection &&
              (*i)->normalize_ring_order() == NULL)
          {
            my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
            null_value= true;
            return 0;
          }

          res+= bg_area<Coordsys>(*i);
          if (null_value)
            return res;
        }

      }
      break;
    default:
      DBUG_ASSERT(false);
      break;
    }
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("st_area");
  }

  /*
    Given a polygon whose rings' points are in counter-clockwise order,
    boost geometry computes an area of negative value. Also, the inner ring
    has to be clockwise.

    We now always make polygon rings CCW --- outer ring CCW and inner rings CW,
    thus if we get a negative value, it's because the inner ring is larger than
    the outer ring, and we should keep it negative.
   */

  return res;
}


double Item_func_area::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return res;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }
  DBUG_ASSERT(geom->get_coordsys() == Geometry::cartesian);

  if (geom->get_geotype() != Geometry::wkb_geometrycollection &&
      geom->normalize_ring_order() == NULL)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_real(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  res= bg_area<bgcs::cartesian>(geom);

  // Had error in bg_area.
  if (null_value)
    return error_real();

  if (!std::isfinite(res))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }
  return res;
}

double Item_func_glength::val_real()
{
  DBUG_ASSERT(fixed == 1);
  double res= 0;				// In case of errors
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  Geometry *geom;

  if ((null_value= (!swkb || args[0]->null_value)))
    return res;
  if (!(geom= Geometry::construct(&buffer, swkb)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }

  if (geom->get_srid() != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(geom->get_srid(), &srs))
      return error_real(); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          geom->get_srid(),
                          func_name());
    }
  }

  if ((null_value= geom->geom_length(&res)))
    return res;
  if (!std::isfinite(res))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_real();
  }
  return res;
}

longlong Item_func_get_srid::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *swkb= args[0]->val_str(&value);
  Geometry_buffer buffer;
  longlong res= 0L;

  if ((null_value= (!swkb || args[0]->null_value)))
    return res;
  if (!Geometry::construct(&buffer, swkb))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_int();
  }

  return (longlong) (uint4korr(swkb->ptr()));
}


String *Item_func_set_srid::val_str(String *str)
{
  String *geometry_str= args[0]->val_str(str);

  Geometry::srid_t srid= 0;
  // Check that the SRID-argument is within range.
  if (validate_srid_arg(args[1], &srid, &null_value, func_name()))
  {
    return error_str();
  }

  null_value|= args[0]->null_value; 

  // Check if either argument is null.
  if (null_value)
  {
    DBUG_ASSERT(maybe_null);
    return nullptr;
  }
  if (str->copy(*geometry_str))
  {
    return error_str();
  }

  Geometry_buffer buffer1;
  // Assume invalid geometry if construction fails.
  if (!(Geometry::construct(&buffer1, str)))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    return error_str();
  }

  if (srid != 0)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(srid, &srs))
    {
      return error_str();
    }

    if (srs == nullptr)
    {
      my_error(ER_SRS_NOT_FOUND, MYF(0), srid);
      return error_str();
    }
  }

  str->write_at_position(0, srid);

  return str;
}


/**
  Compact a geometry collection, making all its points/multipoints into a
  single multipoint object, and all its linestrings/multilinestrings into a
  single multilinestring object; leave polygons and multipolygons as they were.

  @param g the input geometry collection.
  @param gbuf the place to create the new result geometry collection.
  @param str the String buffer to hold data of the result geometry collection.
 */
static const Geometry *
compact_collection(const Geometry *g, Geometry_buffer *gbuf, String *str)
{
  if (g->get_geotype() != Geometry::wkb_geometrycollection)
    return g;

  uint32 wkb_len, wkb_len0;
  char *wkb_start= g->get_cptr();

  wkb_len= wkb_len0= g->get_data_size();
  BG_models<bgcs::cartesian>::Multilinestring mls;
  Geometry_grouper<BG_models<bgcs::cartesian>::Linestring>
    ls_grouper(&mls);
  wkb_scanner(wkb_start, &wkb_len,
              Geometry::wkb_geometrycollection, false, &ls_grouper);

  BG_models<bgcs::cartesian>::Multipoint mpts;
  wkb_len= wkb_len0;
  Geometry_grouper<BG_models<bgcs::cartesian>::Point>
    pt_grouper(&mpts);
  wkb_scanner(wkb_start, &wkb_len,
              Geometry::wkb_geometrycollection, false, &pt_grouper);

  Gis_geometry_collection *ret= new (gbuf) Gis_geometry_collection();
  wkb_len= wkb_len0;
  Geometry_grouper<BG_models<bgcs::cartesian>::Polygon>
    mplgn_grouper(ret, str);
  wkb_scanner(wkb_start, &wkb_len,
              Geometry::wkb_geometrycollection, false, &mplgn_grouper);

  ret->append_geometry(&mls, str);
  ret->append_geometry(&mpts, str);

  return ret;
}


template<typename Num_type>
class Numeric_interval
{
  Num_type left, right;
  bool is_left_open, is_right_open;
  bool is_left_unlimitted, is_right_unlimitted;
public:
  // Construct a boundless interval.
  Numeric_interval() :left(0), right(0),
    is_left_open(false), is_right_open(false),
    is_left_unlimitted(true), is_right_unlimitted(true)
  {
  }

  // Construct a both bounded internal.
  Numeric_interval(const Num_type &l, bool left_open,
                   const Num_type &r, bool right_open) :left(l), right(r),
    is_left_open(left_open), is_right_open(right_open),
    is_left_unlimitted(false), is_right_unlimitted(false)
  {
  }

  // Construct a half boundless half bounded interval.
  // @param is_left true to specify the left boundary and right is boundless;
  //                false to specify the right boundary and left is boundless.
  Numeric_interval(bool is_left, const Num_type &v, bool is_open)
    :left(0), right(0),
    is_left_open(false), is_right_open(false),
    is_left_unlimitted(true), is_right_unlimitted(true)
  {
    if (is_left)
    {
      left= v;
      is_left_open= is_open;
      is_left_unlimitted= false;
    }
    else
    {
      right= v;
      is_right_open= is_open;
      is_right_unlimitted= false;
    }
  }

  /*
    Get left boundary specification.
    @param [out] l takes back left boundary value. if boundless it's intact.
    @param [out] is_open takes back left boundary openness. if boundless
                         it's intact.
    @return true if it's boundless, false if bounded and the two out parameters
                 take back the boundary info.
  */
  bool get_left(Num_type &l, bool &is_open) const
  {
    if (is_left_unlimitted)
      return true;
    l= left;
    is_open= is_left_open;
    return false;
  }


  /*
    Get right boundary specification.
    @param [out] r takes back right boundary value. if boundless it's intact.
    @param [out] is_open takes back right boundary openness. if boundless
                         it's intact.
    @return true if it's boundless, false if bounded and the two out parameters
                 take back the boundary info.
  */
  bool get_right(Num_type &r, bool &is_open) const
  {
    if (is_right_unlimitted)
      return true;
    r= right;
    is_open= is_right_open;
    return false;
  }
};


class Point_coordinate_checker : public WKB_scanner_event_handler
{
  bool has_invalid;

  // Valid x and y coordinate range.
  Numeric_interval<double> x_val, y_val;
public:
  Point_coordinate_checker(Numeric_interval<double> x_range,
                           Numeric_interval<double> y_range)
    :has_invalid(false), x_val(x_range), y_val(y_range)
  {}

  virtual void on_wkb_start(Geometry::wkbByteOrder bo,
                            Geometry::wkbType geotype,
                            const void *wkb, uint32 len, bool has_hdr)
  {
    if (geotype == Geometry::wkb_point)
    {
      Gis_point pt(wkb, POINT_DATA_SIZE,
                   Geometry::Flags_t(Geometry::wkb_point, len), 0);
      double x= pt.get<0>(), y= pt.get<1>();

      double xmin, xmax, ymin, ymax;
      bool xmin_open, ymin_open, xmax_open, ymax_open;

      if ((!x_val.get_left(xmin, xmin_open) &&
           (x < xmin || (xmin_open && x == xmin))) ||
          (!x_val.get_right(xmax, xmax_open) &&
           (x > xmax || (xmax_open && x == xmax))) ||
          (!y_val.get_left(ymin, ymin_open) &&
           (y < ymin || (ymin_open && y == ymin))) ||
          (!y_val.get_right(ymax, ymax_open) &&
           (y > ymax || (ymax_open && y == ymax))))
      {
        has_invalid= true;
        return;
      }
    }
  }


  virtual void on_wkb_end(const void *wkb)
  {
  }

  bool has_invalid_point() const { return has_invalid; }
};


template <typename CoordinateSystem>
struct BG_distance
{};


template <>
struct BG_distance<bg::cs::cartesian>
{
  static double apply(Item_func_distance *item, Geometry *g1, Geometry *g2)
  {
    // Do the actual computation here for the cartesian CS.
    return item->bg_distance<bgcs::cartesian>(g1, g2);
  }
};


template <>
struct BG_distance<bg::cs::spherical_equatorial<bg::degree> >
{
  static double apply(Item_func_distance *item, Geometry *g1, Geometry *g2)
  {
    // Do the actual computation here for the spherical equatorial CS
    // with degree units.
    return item->bg_distance_spherical(g1, g2);
  }
};


double Item_func_distance::val_real()
{
  typedef bgcs::spherical_equatorial<bg::degree> bgcssed;
  double distance= 0;

  DBUG_ENTER("Item_func_distance::val_real");
  DBUG_ASSERT(fixed == 1);

  String *res1= args[0]->val_str(&tmp_value1);
  String *res2= args[1]->val_str(&tmp_value2);
  Geometry_buffer buffer1, buffer2;
  Geometry *g1, *g2;

  if ((null_value= (!res1 || args[0]->null_value ||
                    !res2 || args[1]->null_value)))
    DBUG_RETURN(0.0);

  if (!(g1= Geometry::construct(&buffer1, res1)) ||
      !(g2= Geometry::construct(&buffer2, res2)))
  {
    // If construction fails, we assume invalid input data.
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    DBUG_RETURN(error_real());
  }

  // The two geometry operand must be in the same coordinate system.
  if (g1->get_srid() != g2->get_srid())
  {
    my_error(ER_GIS_DIFFERENT_SRIDS, MYF(0), func_name(),
             g1->get_srid(), g2->get_srid());
    DBUG_RETURN(error_real());
  }

  if ((g1->get_geotype() != Geometry::wkb_geometrycollection &&
       g1->normalize_ring_order() == NULL) ||
      (g2->get_geotype() != Geometry::wkb_geometrycollection &&
       g2->normalize_ring_order() == NULL))
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    DBUG_RETURN(error_real());
  }

  if (g1->get_srid() != 0 && !is_spherical_equatorial)
  {
    Srs_fetcher fetcher(current_thd);
    const dd::Spatial_reference_system *srs= nullptr;
    if (fetcher.acquire(g1->get_srid(), &srs))
      DBUG_RETURN(error_real()); // Error has already been flagged.

    if (srs == nullptr)
    {
      push_warning_printf(current_thd,
                          Sql_condition::SL_WARNING,
                          ER_WARN_SRS_NOT_FOUND,
                          ER_THD(current_thd, ER_WARN_SRS_NOT_FOUND),
                          g1->get_srid(),
                          func_name());
    }
  }

  if (is_spherical_equatorial)
  {
    Geometry::wkbType gt1= g1->get_geotype();
    Geometry::wkbType gt2= g2->get_geotype();
    if (!((gt1 == Geometry::wkb_point || gt1 == Geometry::wkb_multipoint) &&
          (gt2 == Geometry::wkb_point || gt2 == Geometry::wkb_multipoint)))
    {
      my_error(ER_GIS_UNSUPPORTED_ARGUMENT, MYF(0), func_name());
      DBUG_RETURN(error_real());
    }

    if (arg_count == 3)
    {
      earth_radius= args[2]->val_real();
      if (args[2]->null_value)
        DBUG_RETURN(error_real());
      if (earth_radius <= 0)
      {
        my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
        DBUG_RETURN(error_real());
      }
    }

    /*
      Make sure all points' coordinates are valid:
      x in (-180, 180], y in [-90, 90].
    */
    Numeric_interval<double> x_range(-180, true, 180, false);   // (-180, 180]
    Numeric_interval<double> y_range(-90, false, 90, false);    // [-90, 90]
    Point_coordinate_checker checker(x_range, y_range);

    uint32 wkblen= res1->length() - 4;
    wkb_scanner(res1->ptr() + 4, &wkblen, Geometry::wkb_invalid_type,
                true, &checker);
    if (checker.has_invalid_point())
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      DBUG_RETURN(error_real());
    }

    wkblen= res2->length() - 4;
    wkb_scanner(res2->ptr() + 4, &wkblen, Geometry::wkb_invalid_type,
                true, &checker);
    if (checker.has_invalid_point())
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      DBUG_RETURN(error_real());
    }
  }

  if (g1->get_type() != Geometry::wkb_geometrycollection &&
      g2->get_type() != Geometry::wkb_geometrycollection)
  {
    if (is_spherical_equatorial)
      distance= BG_distance<bgcssed>::apply(this, g1, g2);
    else
      distance= BG_distance<bgcs::cartesian>::apply(this, g1, g2);
  }
  else
    distance= geometry_collection_distance(g1, g2);

  if (null_value)
    DBUG_RETURN(error_real());

  if (!std::isfinite(distance) || distance < 0)
  {
    my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
    DBUG_RETURN(error_real());
  }
  DBUG_RETURN(distance);
}


/*
  Calculate the distance of two geometry collections. BG has optimized
  algorithm to calculate distance among multipoints, multilinestrings
  and polygons, so we compact the collection to make a single multipoint,
  a single multilinestring, and the rest are all polygons and multipolygons,
  and do a nested loop to calculate the minimum distances among such
  compacted components as the final result.
 */
double Item_func_distance::
geometry_collection_distance(const Geometry *g1, const Geometry *g2)
{
  BG_geometry_collection bggc1, bggc2;
  bool initialized= false, all_normalized= false;
  double min_distance= DBL_MAX, dist= DBL_MAX;
  String gcstr1, gcstr2;
  Geometry_buffer buf1, buf2;
  const Geometry *g11, *g22;

  g11= compact_collection(g1, &buf1, &gcstr1);
  g22= compact_collection(g2, &buf2, &gcstr2);

  bggc1.fill(g11);
  bggc2.fill(g22);
  for (BG_geometry_collection::Geometry_list::iterator
       i= bggc1.get_geometries().begin();
       i != bggc1.get_geometries().end(); ++i)
  {
    /* Normalize polygon rings, do only once for each component. */
    if ((*i)->get_geotype() != Geometry::wkb_geometrycollection &&
        (*i)->normalize_ring_order() == NULL)
    {
      my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
      return error_real();
    }

    for (BG_geometry_collection::Geometry_list::iterator
         j= bggc2.get_geometries().begin();
         j != bggc2.get_geometries().end(); ++j)
    {
      /* Normalize polygon rings, do only once for each component. */
      if (!all_normalized &&
          (*j)->get_geotype() != Geometry::wkb_geometrycollection &&
          (*j)->normalize_ring_order() == NULL)
      {
        my_error(ER_GIS_INVALID_DATA, MYF(0), func_name());
        return error_real();
      }

      if (is_spherical_equatorial)
      {
        // For now this is never reached because we only support
        // distance([multi]point, [multi]point) for spherical.
        DBUG_ASSERT(false);
      }
      else
        dist= BG_distance<bgcs::cartesian>::apply(this, *i, *j);
      if (null_value)
        return error_real();
      if (dist < 0 || boost::math::isnan(dist))
        return dist;

      if (!initialized)
      {
        min_distance= dist;
        initialized= true;
      }
      else if (min_distance > dist)
        min_distance= dist;

    }

    all_normalized= true;
    if (!initialized)
      break;                                  // bggc2 is empty.
  }

  /*
    If at least one of the collections is empty, we have NULL result.
  */
  if (!initialized)
    return error_real();

  return min_distance;
}


double Item_func_distance::
distance_point_geometry_spherical(const Geometry *g1, const Geometry *g2)
{
  typedef bgcs::spherical_equatorial<bg::degree> bgcssed;
  double res= 0;
  bg::strategy::distance::haversine<double, double>
    dist_strategy(earth_radius);

  BG_models<bgcssed>::Point
    bg1(g1->get_data_ptr(), g1->get_data_size(),
        g1->get_flags(), g1->get_srid());

  switch (g2->get_type())
  {
  case Geometry::wkb_point:
    {
      BG_models<bgcssed>::Point
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2, dist_strategy);
    }
    break;
  case Geometry::wkb_multipoint:
    {
      BG_models<bgcssed>::Multipoint
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());

      res= bg::distance(bg1, bg2, dist_strategy);
    }
    break;
  default:
    DBUG_ASSERT(false);
    break;
  }
  return res;
}


double Item_func_distance::
distance_multipoint_geometry_spherical(const Geometry *g1, const Geometry *g2)
{
  typedef bgcs::spherical_equatorial<bg::degree> bgcssed;
  double res= 0;
  bg::strategy::distance::haversine<double, double>
    dist_strategy(earth_radius);

  BG_models<bgcssed>::Multipoint
    bg1(g1->get_data_ptr(), g1->get_data_size(),
        g1->get_flags(), g1->get_srid());

  switch (g2->get_type())
  {
  case Geometry::wkb_point:
    {
      BG_models<bgcssed>::Point
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2, dist_strategy);
    }
    break;
  case Geometry::wkb_multipoint:
    {
      BG_models<bgcssed>::Multipoint
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2, dist_strategy);
    }
    break;
  default:
    DBUG_ASSERT(false);
    break;
  }

  return res;
}


double Item_func_distance::
bg_distance_spherical(const Geometry *g1, const Geometry *g2)
{
  double res= 0;

  try
  {
    switch (g1->get_type())
    {
    case Geometry::wkb_point:
      res= distance_point_geometry_spherical(g1, g2);
      break;
    case Geometry::wkb_multipoint:
      res= distance_multipoint_geometry_spherical(g1, g2);
      break;
    default:
      DBUG_ASSERT(false);
      break;
    }
  }
  catch (...)
  {
    null_value= true;
    handle_gis_exception("st_distance_sphere");
  }

  return res;
}


template <typename Coordsys, typename BG_geometry>
double Item_func_distance::
distance_dispatch_second_geometry(const BG_geometry& bg1, const Geometry* g2)
{
  double res= 0;
  switch (g2->get_type())
  {
  case Geometry::wkb_point:
    {
      typename BG_models<Coordsys>::Point
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  case Geometry::wkb_multipoint:
    {
      typename BG_models<Coordsys>::Multipoint
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  case Geometry::wkb_linestring:
    {
      typename BG_models<Coordsys>::Linestring
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  case Geometry::wkb_multilinestring:
    {
      typename BG_models<Coordsys>::Multilinestring
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  case Geometry::wkb_polygon:
    {
      typename BG_models<Coordsys>::Polygon
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  case Geometry::wkb_multipolygon:
    {
      typename BG_models<Coordsys>::Multipolygon
        bg2(g2->get_data_ptr(), g2->get_data_size(),
            g2->get_flags(), g2->get_srid());
      res= bg::distance(bg1, bg2);
    }
    break;
  default:
    DBUG_ASSERT(false);
    break;
  }
  return res;
}


/*
  Calculate distance of g1 and g2 using Boost.Geometry. We split the
  implementation into 6 smaller functions according to the type of g1, to
  make all functions smaller in size. Because distance is symmetric, we swap
  parameters if the swapped type combination is already implemented.
 */
template <typename Coordsys>
double Item_func_distance::bg_distance(const Geometry *g1, const Geometry *g2)
{
  double res= 0;
  bool had_except= false;

  try
  {
    switch (g1->get_type())
    {
    case Geometry::wkb_point:
      {
        typename BG_models<Coordsys>::Point
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    case Geometry::wkb_multipoint:
      {
        typename BG_models<Coordsys>::Multipoint
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    case Geometry::wkb_linestring:
      {
        typename BG_models<Coordsys>::Linestring
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    case Geometry::wkb_multilinestring:
      {
        typename BG_models<Coordsys>::Multilinestring
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    case Geometry::wkb_polygon:
      {
        typename BG_models<Coordsys>::Polygon
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    case Geometry::wkb_multipolygon:
      {
        typename BG_models<Coordsys>::Multipolygon
          bg1(g1->get_data_ptr(), g1->get_data_size(),
              g1->get_flags(), g1->get_srid());
        res= distance_dispatch_second_geometry<Coordsys>(bg1, g2);
      }
      break;
    default:
      DBUG_ASSERT(false);
      break;
    }
  }
  catch (...)
  {
    had_except= true;
    handle_gis_exception("st_distance");
  }

  if (had_except)
    return error_real();

  return res;
}


