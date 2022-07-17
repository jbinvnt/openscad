/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef _MSC_VER
// Boost conflicts with MPFR under MSVC (google it)
#include <mpfr.h>
#endif

#include "PolySet.h"
#include "PolySetUtils.h"
#include "printutils.h"
#include "Feature.h"

#include "CGALRenderer.h"
#include "CGAL_OGL_VBOPolyhedron.h"
#include "CGALHybridPolyhedron.h"
#include "VertexStateManager.h"
#ifdef ENABLE_MANIFOLD
#include "ManifoldGeometry.h"
#endif

//#include "Preferences.h"

CGALRenderer::CGALRenderer(const shared_ptr<const class Geometry>& geom)
  : last_render_state(Feature::ExperimentalVxORenderers.is_enabled()) // FIXME: this is temporary to make switching between renderers seamless.
{
  this->addGeometry(geom);
}

void CGALRenderer::addGeometry(const shared_ptr<const Geometry>& geom)
{
  if (const auto geomlist = dynamic_pointer_cast<const GeometryList>(geom)) {
    for (const auto& item : geomlist->getChildren()) {
      this->addGeometry(item.second);
    }
  } else if (const auto ps = dynamic_pointer_cast<const PolySet>(geom)) {
    assert(ps->getDimension() == 3);
    // We need to tessellate here, in case the generated PolySet contains concave polygons
    // See tests/data/scad/3D/features/polyhedron-concave-test.scad
    auto ps_tri = new PolySet(3, ps->convexValue());
    ps_tri->setConvexity(ps->getConvexity());
    PolySetUtils::tessellate_faces(*ps, *ps_tri);
    this->polysets.push_back(shared_ptr<const PolySet>(ps_tri));
  } else if (const auto poly = dynamic_pointer_cast<const Polygon2d>(geom)) {
    this->polysets.push_back(shared_ptr<const PolySet>(poly->tessellate()));
  } else if (const auto new_N = dynamic_pointer_cast<const CGAL_Nef_polyhedron>(geom)) {
    assert(new_N->getDimension() == 3);
    if (!new_N->isEmpty()) {
      auto ps = new PolySet(3);
      bool err = CGALUtils::createPolySetFromNefPolyhedron3(*(new_N->p3), *ps);
      if (err) {
        LOG(message_group::Error, Location::NONE, "", "Nef->PolySet failed");
        return;
      }
      this->polysets.push_back(shared_ptr<const PolySet>(ps));
    }
  } else if (const auto hybrid = dynamic_pointer_cast<const CGALHybridPolyhedron>(geom)) {
    // TODO(ochafik): Implement rendering of CGAL_HybridMesh (CGAL::Surface_mesh) instead.
    this->polysets.push_back(hybrid->toPolySet());
#ifdef ENABLE_MANIFOLD
  } else if (const auto mani = dynamic_pointer_cast<const ManifoldGeometry>(geom)) {
    this->polysets.push_back(mani->toPolySet());
#endif
  } else {
    assert(false && "unsupported geom in CGALRenderer");
  }

  if (!this->nefPolyhedrons.empty() && this->polyhedrons.empty()) createPolyhedrons();
}

CGALRenderer::~CGALRenderer()
{
  if (polyset_vertices_vbo) {
    glDeleteBuffers(1, &polyset_vertices_vbo);
  }
  if (polyset_elements_vbo) {
    glDeleteBuffers(1, &polyset_elements_vbo);
  }
}

void CGALRenderer::createPolyhedrons()
{
  PRINTD("createPolyhedrons");
  this->polyhedrons.clear();

  if (!Feature::ExperimentalVxORenderers.is_enabled()) {
    for (const auto& N : this->nefPolyhedrons) {
      auto p = new CGAL_OGL_Polyhedron(*this->colorscheme);
      CGAL::OGL::Nef3_Converter<CGAL_Nef_polyhedron3>::convert_to_OGLPolyhedron(*N->p3, p);
      // CGAL_NEF3_MARKED_FACET_COLOR <- CGAL_FACE_BACK_COLOR
      // CGAL_NEF3_UNMARKED_FACET_COLOR <- CGAL_FACE_FRONT_COLOR
      p->init();
      this->polyhedrons.push_back(shared_ptr<CGAL_OGL_Polyhedron>(p));
    }
  } else {
    for (const auto& N : this->nefPolyhedrons) {
      auto p = new CGAL_OGL_VBOPolyhedron(*this->colorscheme);
      CGAL::OGL::Nef3_Converter<CGAL_Nef_polyhedron3>::convert_to_OGLPolyhedron(*N->p3, p);
      // CGAL_NEF3_MARKED_FACET_COLOR <- CGAL_FACE_BACK_COLOR
      // CGAL_NEF3_UNMARKED_FACET_COLOR <- CGAL_FACE_FRONT_COLOR
      p->init();
      this->polyhedrons.push_back(shared_ptr<CGAL_OGL_Polyhedron>(p));
    }
  }
  PRINTD("createPolyhedrons() end");
}

// Overridden from Renderer
void CGALRenderer::setColorScheme(const ColorScheme& cs)
{
  PRINTD("setColorScheme");
  Renderer::setColorScheme(cs);
  colormap[ColorMode::CGAL_FACE_2D_COLOR] = ColorMap::getColor(cs, RenderColor::CGAL_FACE_2D_COLOR);
  colormap[ColorMode::CGAL_EDGE_2D_COLOR] = ColorMap::getColor(cs, RenderColor::CGAL_EDGE_2D_COLOR);
  this->polyhedrons.clear(); // Mark as dirty
  PRINTD("setColorScheme done");
}

void CGALRenderer::createPolySets()
{
  PRINTD("createPolySets() polyset");

  polyset_states.clear();

  VertexArray vertex_array(std::make_shared<VertexStateFactory>(), polyset_states);

  VertexStateManager vsm(*this, vertex_array);

  vertex_array.addEdgeData();
  vertex_array.addSurfaceData();
  vertex_array.writeSurface();
  add_shader_data(vertex_array);


  size_t num_vertices = 0;
  if (this->polysets.size()) {
    for (const auto& polyset : this->polysets) {
      num_vertices += getSurfaceBufferSize(*polyset);
      num_vertices += getEdgeBufferSize(*polyset);
    }
  }

  vsm.initializeSize(num_vertices);

  for (const auto& polyset : this->polysets) {
    Color4f color;

    PRINTD("polysets");
    if (polyset->getDimension() == 2) {
      PRINTD("2d polysets");
      vertex_array.writeEdge();

      std::shared_ptr<VertexState> init_state = std::make_shared<VertexState>();
      init_state->glEnd().emplace_back([]() {
        GL_TRACE0("glDisable(GL_LIGHTING)");
        GL_CHECKD(glDisable(GL_LIGHTING));
      });
      polyset_states.emplace_back(std::move(init_state));

      // Create 2D polygons
      getColor(ColorMode::CGAL_FACE_2D_COLOR, color);
      this->create_polygons(*polyset, vertex_array, CSGMODE_NONE, Transform3d::Identity(), color);

      std::shared_ptr<VertexState> edge_state = std::make_shared<VertexState>();
      edge_state->glBegin().emplace_back([]() {
        GL_TRACE0("glDisable(GL_DEPTH_TEST)");
        GL_CHECKD(glDisable(GL_DEPTH_TEST));
      });
      edge_state->glBegin().emplace_back([]() {
        GL_TRACE0("glLineWidth(2)");
        GL_CHECKD(glLineWidth(2));
      });
      polyset_states.emplace_back(std::move(edge_state));

      // Create 2D edges
      getColor(ColorMode::CGAL_EDGE_2D_COLOR, color);
      this->create_edges(*polyset, vertex_array, CSGMODE_NONE, Transform3d::Identity(), color);

      std::shared_ptr<VertexState> end_state = std::make_shared<VertexState>();
      end_state->glBegin().emplace_back([]() {
        GL_TRACE0("glEnable(GL_DEPTH_TEST)");
        GL_CHECKD(glEnable(GL_DEPTH_TEST));
      });
      polyset_states.emplace_back(std::move(end_state));
    } else {
      PRINTD("3d polysets");
      vertex_array.writeSurface();

      // Create 3D polygons
      getColor(ColorMode::MATERIAL, color);
      Color4f last_color = color;
      add_shader_pointers(vertex_array);
      shaderinfo_t shader_info = this->getShader();
      std::shared_ptr<VertexState> color_state = std::make_shared<VBOShaderVertexState>(0, 0, vertex_array.verticesVBO(), vertex_array.elementsVBO());
      color_state->glBegin().emplace_back([shader_info, last_color]() {
        GL_TRACE("glUniform4f(%d, %f, %f, %f, %f)", shader_info.data.csg_rendering.color_area % last_color[0] % last_color[1] % last_color[2] % last_color[3]);
        glUniform4f(shader_info.data.csg_rendering.color_area, last_color[0], last_color[1], last_color[2], last_color[3]); GL_ERROR_CHECK();
        GL_TRACE("glUniform4f(%d, %f, %f, %f, 1.0)", shader_info.data.csg_rendering.color_edge % ((last_color[0] + 1) / 2) % ((last_color[1] + 1) / 2) % ((last_color[2] + 1) / 2));
        glUniform4f(shader_info.data.csg_rendering.color_edge, (last_color[0] + 1) / 2, (last_color[1] + 1) / 2, (last_color[2] + 1) / 2, 1.0); GL_ERROR_CHECK();
      });
      polyset_states.emplace_back(std::move(color_state));
      this->create_surface(*polyset, vertex_array, CSGMODE_NORMAL, Transform3d::Identity(), last_color);
    }
  }

  if (this->polysets.size()) {
    if (Feature::ExperimentalVxORenderersDirect.is_enabled() || Feature::ExperimentalVxORenderersPrealloc.is_enabled()) {
      if (Feature::ExperimentalVxORenderersIndexing.is_enabled()) {
        GL_TRACE0("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)");
        GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
      }
      GL_TRACE0("glBindBuffer(GL_ARRAY_BUFFER, 0)");
      GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, 0));
    }

    vertex_array.createInterleavedVBOs();
    polyset_vertices_vbo = vertex_array.verticesVBO();
    polyset_elements_vbo = vertex_array.elementsVBO();
  }
}

void CGALRenderer::prepare(bool /*showfaces*/, bool /*showedges*/, const shaderinfo_t * /*shaderinfo*/)
{
  PRINTD("prepare()");
  if (!polyset_states.size()) createPolySets();
  if (!this->nefPolyhedrons.empty() &&
      (this->polyhedrons.empty() || Feature::ExperimentalVxORenderers.is_enabled() != last_render_state)) // FIXME: this is temporary to make switching between renderers seamless.
    createPolyhedrons();

  PRINTD("prepare() end");
}

void CGALRenderer::draw(bool showfaces, bool showedges, const shaderinfo_t * shaderinfo) const
{
  PRINTD("draw()");
  if (!Feature::ExperimentalVxORenderers.is_enabled()) {
    #ifndef DISABLE_FIXEDFUNCTION_GL
    for (const auto& polyset : this->polysets) {
      PRINTD("draw() polyset");
      if (polyset->getDimension() == 2) {
        // Draw 2D polygons
        glDisable(GL_LIGHTING);
        setColor(ColorMode::CGAL_FACE_2D_COLOR);

        for (const auto& polygon : polyset->polygons) {
          glBegin(GL_POLYGON);
          for (const auto& p : polygon) {
            glVertex3d(p[0], p[1], 0);
          }
          glEnd();
        }

        // Draw 2D edges
        glDisable(GL_DEPTH_TEST);

        glLineWidth(2);
        setColor(ColorMode::CGAL_EDGE_2D_COLOR);
        this->render_edges(*polyset, CSGMODE_NONE);
        glEnable(GL_DEPTH_TEST);
      } else {
        // Draw 3D polygons
        setColor(ColorMode::MATERIAL);
        this->render_surface(*polyset, CSGMODE_NORMAL, Transform3d::Identity(), nullptr);
      }
    }
    #endif //DISABLE_FIXEDFUNCTION_GL
  } else {
    GLint prev_id;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_id);
    PRINTDB("Previously, was using shader ID: %d\n", prev_id);
    if(!shaderinfo) {
      PRINTD("Fetching shaderinfo\n");
      shaderinfo = &getShader();
    }
    glUseProgram(shaderinfo->progid);
    GL_ERROR_CHECK();
    GLint new_id;
    glGetIntegerv(GL_CURRENT_PROGRAM, &new_id);
    PRINTDB("Now, using shader ID: %d\n", new_id);
    // grab current state to restore after
    GLfloat current_point_size, current_line_width;
    #ifndef DISABLE_FIXEDFUNCTION_GL
    GLboolean origVertexArrayState = glIsEnabled(GL_VERTEX_ARRAY);
    GLboolean origNormalArrayState = glIsEnabled(GL_NORMAL_ARRAY);
    GLboolean origColorArrayState = glIsEnabled(GL_COLOR_ARRAY);
    #endif //DISABLE_FIXEDFUNCTION_GL

    GL_CHECKD(glGetFloatv(GL_POINT_SIZE, &current_point_size));
    GL_CHECKD(glGetFloatv(GL_LINE_WIDTH, &current_line_width));

    for (const auto& polyset : polyset_states) {
      if (polyset) polyset->draw();
    }

    // restore states
    GL_TRACE("glPointSize(%d)", current_point_size);
    GL_CHECKD(glPointSize(current_point_size));
    GL_TRACE("glLineWidth(%d)", current_line_width);
    GL_CHECKD(glLineWidth(current_line_width));
    #ifndef DISABLE_FIXEDFUNCTION_GL
    if (!origVertexArrayState) glDisableClientState(GL_VERTEX_ARRAY);
    if (!origNormalArrayState) glDisableClientState(GL_NORMAL_ARRAY);
    if (!origColorArrayState) glDisableClientState(GL_COLOR_ARRAY);
    #endif //DISABLE_FIXEDFUNCTION_GL
    glUseProgram(prev_id);
  }

  for (const auto& p : this->getPolyhedrons()) {
    *const_cast<bool *>(&last_render_state) = Feature::ExperimentalVxORenderers.is_enabled(); // FIXME: this is temporary to make switching between renderers seamless.
    if (showfaces) p->set_style(SNC_BOUNDARY);
    else p->set_style(SNC_SKELETON);
    p->draw(showfaces && showedges);
  }

  PRINTD("draw() end");
}

BoundingBox CGALRenderer::getBoundingBox() const
{
  BoundingBox bbox;

  for (const auto& p : this->getPolyhedrons()) {
    CGAL::Bbox_3 cgalbbox = p->bbox();
    bbox.extend(BoundingBox(
                  Vector3d(cgalbbox.xmin(), cgalbbox.ymin(), cgalbbox.zmin()),
                  Vector3d(cgalbbox.xmax(), cgalbbox.ymax(), cgalbbox.zmax())));
  }
  for (const auto& ps : this->polysets) {
    bbox.extend(ps->getBoundingBox());
  }
  return bbox;
}
