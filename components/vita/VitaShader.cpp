#ifdef __vita__

#include "VitaShader.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

#include <osg/Node>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/Uniform>

namespace Vita
{

    // VitaLit Shaders
    // Handles objects, actors, sky, batched statics.
    // Per-vertex (Gouraud) lighting with sun + ambient, linear fog, alpha test, single diffuse texture.

    static const char* s_litVertSource =
        "uniform mat4 osg_ModelViewProjectionMatrix;\n"
        "uniform mat4 osg_ModelViewMatrix;\n"
        "uniform mat3 osg_NormalMatrix;\n"
        "\n"
        "uniform vec3 u_sunDirView;\n"
        "uniform vec3 u_sunColor;\n"
        "uniform vec3 u_ambient;\n"
        "uniform float u_fogStart;\n"
        "uniform float u_fogEnd;\n"
        "\n"
        "uniform int colorMode;\n"
        "uniform vec4 u_material[3];\n"
        "\n"
        // 2 lights packed as pos,diffuse,atten triples: one uniform apply.
        "uniform vec4 u_lights[6];\n"
        "\n"
        "uniform float u_skyHorizonBlend;\n"
        // for waterfalls and other texture-scrolling effects. Defaults to identity
        "uniform mat4 u_texMat0;\n"
        "\n"
        "attribute vec4 osg_Vertex;\n"
        "attribute vec3 osg_Normal;\n"
        "attribute vec4 osg_Color;\n"
        "attribute vec2 osg_MultiTexCoord0;\n"
        "\n"
        "varying vec2 v_texCoord;\n"
        "varying vec4 v_color;\n"
        "varying float v_fogFactor;\n"
        "varying float v_skyHorizon;\n"
        "\n"
        "vec3 calcPointLight(vec3 pos, vec4 lightPos, vec4 lightDiff, vec4 lightAtt, vec3 normal, vec3 matDiff) {\n"
        "    // Early exit if light is inactive (alpha = 0 means no light)\n"
        "    if (lightDiff.a < 0.001) return vec3(0.0);\n"
        "    \n"
        "    vec3 pl = lightPos.xyz - pos;\n"
        "    float pd = length(pl) + 0.001;\n"
        "    float pa = 1.0 / (lightAtt.x + lightAtt.y * pd + lightAtt.z * pd * pd);\n"
        "    return lightDiff.rgb * matDiff * max(dot(normal, pl / pd), 0.0) * pa;\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;\n"
        "    vec3 viewPos = (osg_ModelViewMatrix * osg_Vertex).xyz;\n"
        "    vec3 viewNormal = normalize(osg_NormalMatrix * osg_Normal);\n"
        "\n"
        "    vec3 emission = u_material[2].rgb;\n"
        "    vec4 matDiffuse = u_material[0];\n"
        "    vec4 matAmbient = u_material[1];\n"
        "\n"
        "    if (colorMode == 1) {\n"
        "        emission = osg_Color.rgb;\n"
        "    } else if (colorMode == 2) {\n"
        "        matDiffuse = osg_Color;\n"
        "        matAmbient = osg_Color;\n"
        "    } else if (colorMode == 3) {\n"
        "        matAmbient = osg_Color;\n"
        "    } else if (colorMode == 4) {\n"
        "        matDiffuse = osg_Color;\n"
        "    }\n"
        "\n"
        "    float NdotL = max(dot(viewNormal, u_sunDirView), 0.0);\n"
        // Floor on ambient so dark caves stay navigable even with the 2-light cap.
        "    vec3 effAmbient = max(u_ambient * 1.4, vec3(0.22));\n"
        "    vec3 lighting = emission\n"
        "                  + effAmbient * matAmbient.rgb\n"
        "                  + u_sunColor * matDiffuse.rgb * NdotL;\n"
        "\n"
        "    lighting += calcPointLight(viewPos, u_lights[0], u_lights[1], u_lights[2], viewNormal, matDiffuse.rgb);\n"
        "    lighting += calcPointLight(viewPos, u_lights[3], u_lights[4], u_lights[5], viewNormal, matDiffuse.rgb);\n"
        "\n"
        "    v_color = vec4(min(lighting, vec3(1.0)), matDiffuse.a);\n"
        "    v_texCoord = (u_texMat0 * vec4(osg_MultiTexCoord0, 0.0, 1.0)).xy;\n"
        "\n"
        // View-space z for fog: skips the sqrt of length(viewPos).
        "    float dist = -viewPos.z;\n"
        "    float t = clamp(max(0.0, dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
        "    v_fogFactor = 1.0 - smoothstep(0.0, 1.0, t);\n"
        // Gate expensive horizon math to sky only.
        "    if (u_skyHorizonBlend > 0.0) {\n"
        "        float alt = normalize(osg_Vertex.xyz + vec3(0.0, 0.0, 0.001)).z;\n"
        "        v_skyHorizon = clamp(1.0 - alt * 3.0, 0.0, 1.0);\n"
        "    } else {\n"
        "        v_skyHorizon = 0.0;\n"
        "    }\n"
        "}\n";

    static const char* s_litFragSource =
        "uniform sampler2D diffuseMap;\n"
        "uniform float alphaRef;\n"
        "\n"
        "varying vec2 v_texCoord;\n"
        "varying vec4 v_color;\n"
        "varying float v_fogFactor;\n"
        "varying float v_skyHorizon;\n"
        "\n"
        "uniform vec3 u_fogColorEffective;\n"
        "void main() {\n"
        "    vec4 tex = texture2D(diffuseMap, v_texCoord);\n"
        "    vec4 color = vec4(tex.rgb * v_color.rgb, tex.a * v_color.a);\n"
        "    if (color.a < alphaRef) discard;\n"
        "    vec3 fogged = mix(u_fogColorEffective, color.rgb, v_fogFactor);\n"
        "    fogged = mix(fogged, u_fogColorEffective, v_skyHorizon);\n"
        "    gl_FragColor = vec4(fogged, color.a);\n"
        "}\n";

    //VitaTerrain Shaders
    // Handles terrain chunks. Same lighting as VitaLit but adds blend map sampling
    // on texture unit 1 and texture matrix uniforms for UV scaling.

    static const char* s_terrainVertSource =
        "uniform mat4 osg_ModelViewProjectionMatrix;\n"
        "uniform mat4 osg_ModelViewMatrix;\n"
        "uniform mat3 osg_NormalMatrix;\n"
        "\n"
        "uniform vec3 u_sunDirView;\n"
        "uniform vec3 u_sunColor;\n"
        "uniform vec3 u_ambient;\n"
        "uniform float u_fogStart;\n"
        "uniform float u_fogEnd;\n"
        "\n"
        "uniform int colorMode;\n"
        "uniform vec4 u_material[3];\n"
        "\n"
        "uniform mat4 u_texMat0;\n"
        "uniform mat4 u_texMat1;\n"
        "\n"
        "attribute vec4 osg_Vertex;\n"
        "attribute vec3 osg_Normal;\n"
        "attribute vec4 osg_Color;\n"
        "attribute vec2 osg_MultiTexCoord0;\n"
        "attribute vec2 osg_MultiTexCoord1;\n"
        "\n"
        "varying vec2 v_texCoord;\n"
        "varying vec2 v_texCoord2;\n"
        "varying vec4 v_color;\n"
        "varying float v_fogFactor;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;\n"
        "    vec3 viewPos = (osg_ModelViewMatrix * osg_Vertex).xyz;\n"
        "    vec3 viewNormal = normalize(osg_NormalMatrix * osg_Normal);\n"
        "\n"
        "    vec3 emission = u_material[2].rgb;\n"
        "    vec4 matDiffuse = u_material[0];\n"
        "    vec4 matAmbient = u_material[1];\n"
        "\n"
        "    if (colorMode == 1) {\n"
        "        emission = osg_Color.rgb;\n"
        "    } else if (colorMode == 2) {\n"
        "        matDiffuse = osg_Color;\n"
        "        matAmbient = osg_Color;\n"
        "    } else if (colorMode == 3) {\n"
        "        matAmbient = osg_Color;\n"
        "    } else if (colorMode == 4) {\n"
        "        matDiffuse = osg_Color;\n"
        "    }\n"
        "\n"
        "    float NdotL = max(dot(viewNormal, u_sunDirView), 0.0);\n"
        // Floor on ambient so dark caves stay navigable even with the 2-light cap.
        "    vec3 effAmbient = max(u_ambient * 1.4, vec3(0.22));\n"
        "    vec3 lighting = emission\n"
        "                  + effAmbient * matAmbient.rgb\n"
        "                  + u_sunColor * matDiffuse.rgb * NdotL;\n"
        "\n"
        "    v_color = vec4(min(lighting, vec3(1.0)), matDiffuse.a);\n"
        "    v_texCoord = (u_texMat0 * vec4(osg_MultiTexCoord0, 0.0, 1.0)).xy;\n"
        "    v_texCoord2 = (u_texMat1 * vec4(osg_MultiTexCoord1, 0.0, 1.0)).xy;\n"
        "\n"
        "    float dist = length(viewPos);\n"
        "    float t = clamp(max(0.0, dist - u_fogStart) / max(u_fogEnd - u_fogStart, 1.0), 0.0, 1.0);\n"
        "    v_fogFactor = 1.0 - smoothstep(0.0, 1.0, t);\n"
        "}\n";

    static const char* s_terrainFragSource =
        "uniform sampler2D diffuseMap;\n"
        "uniform sampler2D blendMap;\n"
        "uniform int u_hasBlendMap;\n"
        "uniform vec4 u_fogColor;\n"
        "\n"
        "varying vec2 v_texCoord;\n"
        "varying vec2 v_texCoord2;\n"
        "varying vec4 v_color;\n"
        "varying float v_fogFactor;\n"
        "\n"
        "uniform vec3 u_fogColorEffective;\n"
        "void main() {\n"
        "    vec4 tex = texture2D(diffuseMap, v_texCoord);\n"
        "    float alpha = 1.0;\n"
        "    if (u_hasBlendMap != 0)\n"
        "        alpha = texture2D(blendMap, v_texCoord2).a;\n"
        "    vec4 color = vec4(tex.rgb * v_color.rgb, alpha);\n"
        "    gl_FragColor = mix(vec4(u_fogColorEffective, u_fogColor.a), color, v_fogFactor);\n"
        "}\n";

    // VitaTerrainMulti Shaders
    // Single-pass terrain: samples up to 4 diffuse layers + 3 blendmaps.
    // Reduces terrain draw calls from N-per-chunk to 1-per-chunk.
    // Vertex shader is same as VitaTerrain (one diffuse UV + one blend UV).

    static const char* s_terrainMultiFragSource =
        "uniform sampler2D u_layer0;\n"
        "uniform sampler2D u_layer1;\n"
        "uniform sampler2D u_layer2;\n"
        "uniform sampler2D u_layer3;\n"
        "uniform sampler2D u_blend1;\n"
        "uniform sampler2D u_blend2;\n"
        "uniform sampler2D u_blend3;\n"
        "uniform int u_numLayers;\n"
        "uniform vec4 u_fogColor;\n"
        "uniform vec3 u_fogColorEffective;\n"
        "\n"
        "varying vec2 v_texCoord;\n"
        "varying vec2 v_texCoord2;\n"
        "varying vec4 v_color;\n"
        "varying float v_fogFactor;\n"
        "\n"
        "void main() {\n"
        "    vec3 color = texture2D(u_layer0, v_texCoord).rgb;\n"
        "    if (u_numLayers > 1) {\n"
        "        float b1 = texture2D(u_blend1, v_texCoord2).a;\n"
        "        color = mix(color, texture2D(u_layer1, v_texCoord).rgb, b1);\n"
        "    }\n"
        "    if (u_numLayers > 2) {\n"
        "        float b2 = texture2D(u_blend2, v_texCoord2).a;\n"
        "        color = mix(color, texture2D(u_layer2, v_texCoord).rgb, b2);\n"
        "    }\n"
        "    if (u_numLayers > 3) {\n"
        "        float b3 = texture2D(u_blend3, v_texCoord2).a;\n"
        "        color = mix(color, texture2D(u_layer3, v_texCoord).rgb, b3);\n"
        "    }\n"
        "    color *= v_color.rgb;\n"
        "    gl_FragColor = mix(vec4(u_fogColorEffective, u_fogColor.a), vec4(color, 1.0), v_fogFactor);\n"
        "}\n";

    // ==================== Program Creation ====================

    osg::ref_ptr<osg::Program> createVitaLitProgram()
    {
        static osg::ref_ptr<osg::Program> s_program;
        if (s_program)
            return s_program;

        s_program = new osg::Program;
        s_program->setName("VitaLit");
        s_program->addShader(new osg::Shader(osg::Shader::VERTEX, s_litVertSource));
        s_program->addShader(new osg::Shader(osg::Shader::FRAGMENT, s_litFragSource));

        // Do NOT add explicit addBindAttribLocation() calls here.
        // OSG's State already provides all attribute bindings when
        // setUseVertexAttributeAliasing(true) is enabled, and vitaGL's
        // glsl_attr_map only holds 16 entries — our explicit bindings
        // combined with State's 13 bindings would overflow that buffer.

        return s_program;
    }

    osg::ref_ptr<osg::Program> createVitaTerrainProgram()
    {
        static osg::ref_ptr<osg::Program> s_program;
        if (s_program)
            return s_program;

        s_program = new osg::Program;
        s_program->setName("VitaTerrain");
        s_program->addShader(new osg::Shader(osg::Shader::VERTEX, s_terrainVertSource));
        s_program->addShader(new osg::Shader(osg::Shader::FRAGMENT, s_terrainFragSource));

        // Do NOT add explicit addBindAttribLocation() calls here.
        // OSG's State provides all attribute bindings via vertex attribute aliasing.
        // See VitaLit comment above for details on the vitaGL buffer overflow.

        return s_program;
    }

    osg::ref_ptr<osg::Program> createVitaTerrainMultiProgram()
    {
        static osg::ref_ptr<osg::Program> s_program;
        if (s_program)
            return s_program;

        s_program = new osg::Program;
        s_program->setName("VitaTerrainMulti");
        // Reuse VitaTerrain vertex shader — same UV handling
        s_program->addShader(new osg::Shader(osg::Shader::VERTEX, s_terrainVertSource));
        s_program->addShader(new osg::Shader(osg::Shader::FRAGMENT, s_terrainMultiFragSource));

        return s_program;
    }

    // ==================== Shader Application ====================

    osg::ref_ptr<osg::Texture2D> getWhiteFallbackTexture()
    {
        static osg::ref_ptr<osg::Texture2D> s_white;
        if (s_white)
            return s_white;
        osg::ref_ptr<osg::Image> img = new osg::Image;
        static unsigned char white[] = { 255, 255, 255, 255 };
        img->setImage(1, 1, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, white, osg::Image::NO_DELETE);
        s_white = new osg::Texture2D(img);
        s_white->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        s_white->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        s_white->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        s_white->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        return s_white;
    }

    osg::Uniform* internUniform(osg::Uniform* fresh)
    {
        osg::ref_ptr<osg::Uniform> holder(fresh);
        std::string key = fresh->getName();
        const unsigned int type = (unsigned int)fresh->getType();
        key.append(reinterpret_cast<const char*>(&type), sizeof(type));
        if (const osg::FloatArray* fa = fresh->getFloatArray())
            key.append(static_cast<const char*>(fa->getDataPointer()), fa->getTotalDataSize());
        else if (const osg::IntArray* ia = fresh->getIntArray())
            key.append(static_cast<const char*>(ia->getDataPointer()), ia->getTotalDataSize());
        else if (const osg::UIntArray* ua = fresh->getUIntArray())
            key.append(static_cast<const char*>(ua->getDataPointer()), ua->getTotalDataSize());
        else
            return holder.release(); // unkeyed data type; skip interning

        static std::map<std::string, osg::ref_ptr<osg::Uniform>> s_pool;
        static std::mutex s_poolMutex;
        std::lock_guard<std::mutex> lock(s_poolMutex);
        auto it = s_pool.try_emplace(std::move(key), holder).first;
        return it->second.get();
    }

    void applyVitaShader(osg::Node& node, int colorMode, float alphaRef, const osg::Material* mat)
    {
        osg::StateSet* ss = node.getOrCreateStateSet();
        ss->setAttributeAndModes(createVitaLitProgram(),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        // Group VitaLit draws together so SGX543 doesn't reload USSE program state
        // between interleaved terrain/object draws. Skip if the mesh is already
        // in a transparent/sorted bin (NiAlphaProperty sets bin 10 / TRANSPARENT_BIN
        // for blended meshes — overriding that would break alpha blending order).
        if (ss->getRenderingHint() != osg::StateSet::TRANSPARENT_BIN
            && ss->getBinNumber() == 0)
        {
            ss->setRenderBinDetails(1, "RenderBin");
        }
        ss->addUniform(internUniform(new osg::Uniform("colorMode", colorMode)));
        ss->addUniform(internUniform(new osg::Uniform("alphaRef", alphaRef)));
        ss->addUniform(internUniform(new osg::Uniform("diffuseMap", 0)));
        osg::Uniform* material = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "u_material", 3);
        material->setElement(0u, mat ? mat->getDiffuse(osg::Material::FRONT) : osg::Vec4f(1, 1, 1, 1));
        material->setElement(1u, mat ? mat->getAmbient(osg::Material::FRONT) : osg::Vec4f(1, 1, 1, 1));
        material->setElement(2u, mat ? mat->getEmission(osg::Material::FRONT) : osg::Vec4f(0, 0, 0, 1));
        ss->addUniform(internUniform(material));
    }

    // Scene Uniforms

    void setupSceneUniforms(osg::StateSet* ss)
    {
        ss->addUniform(new osg::Uniform("u_sunDirView", osg::Vec3f(0.f, 0.f, 1.f)));
        ss->addUniform(new osg::Uniform("u_sunColor", osg::Vec3f(1.f, 1.f, 1.f)));
        ss->addUniform(new osg::Uniform("u_ambient", osg::Vec3f(0.3f, 0.3f, 0.3f)));
        ss->addUniform(new osg::Uniform("u_fogStart", 0.f));
        ss->addUniform(new osg::Uniform("u_fogEnd", 2048.f));
        ss->addUniform(new osg::Uniform("u_fogColor", osg::Vec4f(0.7f, 0.7f, 0.7f, 1.f)));
        // Precomputed fog tint = mix(u_fogColor.rgb, vec3(0.08), greyMix). Keeps
        // a smoothstep + mix out of the fragment shader; updated CPU-side in
        // updateSceneUniforms whenever u_fogColor changes.
        ss->addUniform(new osg::Uniform("u_fogColorEffective", osg::Vec3f(0.7f, 0.7f, 0.7f)));

        // Default material uniforms (overridden per-node by applyVitaShader)
        {
            osg::Uniform* material = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "u_material", 3);
            material->setElement(0u, osg::Vec4f(1, 1, 1, 1));
            material->setElement(1u, osg::Vec4f(1, 1, 1, 1));
            material->setElement(2u, osg::Vec4f(0, 0, 0, 1));
            ss->addUniform(material);
        }

        // Default packed light array — all slots inactive.
        {
            osg::Uniform* lights = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "u_lights", 6);
            for (unsigned int i = 0; i < 2; ++i)
            {
                lights->setElement(i * 3 + 0, osg::Vec4f(0, 0, 0, 0));
                lights->setElement(i * 3 + 1, osg::Vec4f(0, 0, 0, 0));
                lights->setElement(i * 3 + 2, osg::Vec4f(1, 0, 0, 0));
            }
            ss->addUniform(lights);
        }

        // Default terrain uniforms
        ss->addUniform(new osg::Uniform("u_texMat0", osg::Matrixf::identity()));
        ss->addUniform(new osg::Uniform("u_texMat1", osg::Matrixf::identity()));
        ss->addUniform(new osg::Uniform("u_hasBlendMap", 0));
        ss->addUniform(new osg::Uniform("u_numLayers", 1));

        // 0 = off (default), 1 = on (set on sky root only)
        ss->addUniform(new osg::Uniform("u_skyHorizonBlend", 0.f));
    }

    void updateSceneUniforms(osg::StateSet* ss, const osg::Vec3f& sunDirView, const osg::Vec3f& sunColor,
        const osg::Vec3f& ambient, float fogStart, float fogEnd, const osg::Vec4f& fogColor)
    {
        // u_sunDirView changes every frame the camera rotates — always update.
        if (osg::Uniform* u = ss->getUniform("u_sunDirView"))
            u->set(sunDirView);

        // The remaining scene uniforms only change at sub-Hz rates (weather
        // transitions, fog controller ticks). Cache last-set values and skip
        // unchanged writes — saves several Uniform::set passes per frame
        // through the OSG → vitaGL → SceGxm path. Function is called from
        // a single StateSet (mSceneRoot) on the main thread, so static cache
        // is safe.
        static osg::Vec3f s_sunColor(-1.f, -1.f, -1.f);
        static osg::Vec3f s_ambient(-1.f, -1.f, -1.f);
        static float s_fogStart = -1.f;
        static float s_fogEnd = -1.f;
        static osg::Vec4f s_fogColor(-1.f, -1.f, -1.f, -1.f);

        if (sunColor != s_sunColor)
        {
            if (osg::Uniform* u = ss->getUniform("u_sunColor"))
                u->set(sunColor);
            s_sunColor = sunColor;
        }
        if (ambient != s_ambient)
        {
            if (osg::Uniform* u = ss->getUniform("u_ambient"))
                u->set(ambient);
            s_ambient = ambient;
        }
        if (fogStart != s_fogStart)
        {
            if (osg::Uniform* u = ss->getUniform("u_fogStart"))
                u->set(fogStart);
            s_fogStart = fogStart;
        }
        if (fogEnd != s_fogEnd)
        {
            if (osg::Uniform* u = ss->getUniform("u_fogEnd"))
                u->set(fogEnd);
            s_fogEnd = fogEnd;
        }
        if (fogColor != s_fogColor)
        {
            if (osg::Uniform* u = ss->getUniform("u_fogColor"))
                u->set(fogColor);

            // Precompute fogRGB = mix(fogColor.rgb, vec3(0.08), 1-smoothstep(0.05,0.20,fogLum))
            // — was previously per-fragment in 3 shaders.
            const float fogLum = std::max({ fogColor.r(), fogColor.g(), fogColor.b() });
            const float t = std::clamp((fogLum - 0.05f) / (0.20f - 0.05f), 0.f, 1.f);
            const float smoothLum = t * t * (3.f - 2.f * t);
            const float fogGreyMix = 1.f - smoothLum;
            const osg::Vec3f fogRGB(
                fogColor.r() * (1.f - fogGreyMix) + 0.08f * fogGreyMix,
                fogColor.g() * (1.f - fogGreyMix) + 0.08f * fogGreyMix,
                fogColor.b() * (1.f - fogGreyMix) + 0.08f * fogGreyMix);
            if (osg::Uniform* u = ss->getUniform("u_fogColorEffective"))
                u->set(fogRGB);

            s_fogColor = fogColor;
        }
    }

} // namespace Vita

#endif // __vita__
