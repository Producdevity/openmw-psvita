#ifndef OPENMW_COMPONENTS_VITA_SHADER_H
#define OPENMW_COMPONENTS_VITA_SHADER_H

#ifdef __vita__

#include <osg/Material>
#include <osg/Program>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/ref_ptr>

namespace Vita
{
    /// Get the shared VitaLit program (objects, actors, sky, batched statics).
    osg::ref_ptr<osg::Program> createVitaLitProgram();

    /// Get a 1x1 white fallback texture (for untextured geometry).
    osg::ref_ptr<osg::Texture2D> getWhiteFallbackTexture();

    /// Get the shared VitaTerrain program (terrain chunks with blend maps, per-pass).
    osg::ref_ptr<osg::Program> createVitaTerrainProgram();

    /// Get the shared VitaTerrainMulti program (up to 4 layers in single pass).
    osg::ref_ptr<osg::Program> createVitaTerrainMultiProgram();

    /// Apply VitaLit shader + material uniforms to a node.
    /// Called from ShaderVisitor for each geometry/drawable node.
    void applyVitaShader(osg::Node& node, int colorMode, float alphaRef, const osg::Material* mat);

    /// Canonicalize by (name, type, value) so identical uniforms share one
    /// instance and the per-program apply memo can skip re-uploads.
    /// Interned uniforms must never be mutated in place.
    osg::Uniform* internUniform(osg::Uniform* fresh);

    /// Add lighting/fog/camera uniforms to the scene root StateSet.
    /// These are updated per-frame by RenderingManager.
    void setupSceneUniforms(osg::StateSet* sceneRootSS);

    /// Update per-frame uniforms on the scene root StateSet.
    /// sunDirView is the sun direction in view (eye) space.
    void updateSceneUniforms(osg::StateSet* ss, const osg::Vec3f& sunDirView, const osg::Vec3f& sunColor,
        const osg::Vec3f& ambient, float fogStart, float fogEnd, const osg::Vec4f& fogColor);
}

#endif // __vita__
#endif // OPENMW_COMPONENTS_VITA_SHADER_H
