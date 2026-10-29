// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

internal static class StaticMapGeometryTests {
    static void Check(bool value, string why) {
        if (!value) throw new Exception(why);
    }

    static void Near(Vector3 actual, Vector3 expected, string why) {
        Check(Vector3.Distance(actual, expected) < .00003f, $"{why}: expected {expected}, got {actual}");
    }

    static void Reject(Action action, string why) {
        try { action(); }
        catch (InvalidDataException) { return; }
        throw new Exception(why);
    }

    static FLVER.Vertex Vertex(int paletteIndex) {
        var vertex = new FLVER.Vertex {
            Position = new Vector3(1, 2, 3),
            Normal = Vector3.Normalize(new Vector3(1, 2, 3)),
            NormalW = 127
        };
        vertex.BoneIndices[0] = paletteIndex;
        vertex.BoneWeights[0] = .75f;
        vertex.Tangents.Add(new Vector4(Vector3.Normalize(new Vector3(2, -1, 0)), -1));
        vertex.UVs.Add(new Vector3(.25f, .75f, 0));
        vertex.Colors.Add(new FLVER.VertexColor(.5f, .1f, .2f, .3f));
        return vertex;
    }

    static (FLVER2 Source, FLVER2.Mesh Mesh) Fixture() {
        var source = new FLVER2();
        source.Nodes.Add(new FLVER.Node {
            Name = "scaled-root", ParentIndex = -1,
            Scale = new Vector3(2, 3, 4), Translation = new Vector3(10, 20, 30)
        });
        source.Nodes.Add(new FLVER.Node {
            Name = "rotated-root", ParentIndex = -1,
            Scale = Vector3.One, Rotation = new Vector3(0, MathF.PI / 2, 0),
            Translation = new Vector3(-10, 5, 7)
        });
        source.Nodes.Add(new FLVER.Node {
            Name = "scaled-child", ParentIndex = 0,
            Scale = new Vector3(.5f, 1, 2), Rotation = new Vector3(0, 0, MathF.PI / 2),
            Translation = new Vector3(1, 2, 3)
        });
        // DS1R files use per-vertex BoneIndices[0] while NormalW is commonly
        // 127 and NodeIndex may exceed the node count. Deliberately reorder the
        // palette so that neither value nor a global-index shortcut can work.
        var mesh = new FLVER2.Mesh { UseBoneWeights = false, NodeIndex = 10 };
        mesh.BoneIndices.AddRange([2, 0, 1]);
        source.Meshes.Add(mesh);
        return (source, mesh);
    }

    static void RigidPalette() {
        var (source, mesh) = Fixture();
        // These coordinates are calculated directly from scale, quarter turns,
        // and translation; they do not call the production world-matrix code.
        Vector3[] positions = [new(8, 27.5f, 66), new(12, 26, 42), new(-7, 7, 6)];
        Vector3[] normals = [
            Vector3.Normalize(new(-1, 2f / 3, .375f)),
            Vector3.Normalize(new(.5f, 2f / 3, .75f)),
            Vector3.Normalize(new(3, 2, -1))
        ];
        Vector3[] tangents = [
            Vector3.Normalize(new(2, 3, 0)),
            Vector3.Normalize(new(4, -3, 0)),
            Vector3.Normalize(new(0, -1, -2))
        ];
        foreach (var profile in new[] { "DS1", "DS1R" }) {
            var geometry = new StaticMapGeometry(source, profile);
            for (int paletteIndex = 0; paletteIndex < 3; ++paletteIndex) {
                var input = Vertex(paletteIndex);
                var baked = geometry.Bake(mesh, input);
                Near(baked.Position, positions[paletteIndex], profile + " rigid palette/world position");
                Near(baked.Normal, normals[paletteIndex], profile + " inverse-transpose normal");
                Near(new(baked.Tangents[0].X, baked.Tangents[0].Y, baked.Tangents[0].Z), tangents[paletteIndex], profile + " transformed tangent");
                Check(baked.Tangents[0].W == -1, "Tangent handedness changed");
                Check(baked.Tangents.Count >= 2 && baked.UVs.Count >= 2, "Native map vertex streams were not padded");
                Near(baked.UVs[0], input.UVs[0], "Source UV changed");
                foreach (var tangent in baked.Tangents) {
                    var direction = new Vector3(tangent.X, tangent.Y, tangent.Z);
                    Check(MathF.Abs(direction.Length() - 1) < .00003f, "Tangent is not normalized");
                    Check(MathF.Abs(Vector3.Dot(direction, baked.Normal)) < .00003f, "Tangent is not perpendicular to the normal");
                }
                Check(baked.NormalW == 0, "Native map vertex retains a rigid bone selector");
                for (int i = 0; i < 4; ++i)
                    Check(baked.BoneIndices[i] == 0 && baked.BoneWeights[i] == 0, "Native map vertex retains source skin data");
                Check(baked.Colors.Count >= 2 && baked.Colors.All(c => c.A == 1 && c.R == 1 && c.G == 1 && c.B == 1), "Native vertex colors are not white");
                Near(input.Position, new(1, 2, 3), "Baking mutated the source position");
                Check(input.NormalW == 127 && input.BoneIndices[0] == paletteIndex && input.BoneWeights[0] == .75f && input.Tangents.Count == 1 && input.UVs.Count == 1 && input.Colors.Count == 1 && input.Colors[0].A == .5f, "Baking mutated source vertex streams");
            }
            Reject(() => geometry.Bake(mesh, Vertex(3)), "Out-of-range DS1 rigid palette index accepted");
            Reject(() => geometry.Bake(mesh, Vertex(-1)), "Negative DS1 rigid palette index accepted");
            mesh.BoneIndices[0] = 99;
            Reject(() => geometry.Bake(mesh, Vertex(0)), "Out-of-range DS1 global node accepted");
            mesh.BoneIndices[0] = 2;
        }
    }

    static void OtherSkinModes() {
        var (source, mesh) = Fixture();
        foreach (var profile in new[] { "DS2", "DS2S" }) {
            var input = Vertex(0);
            input.NormalW = 1;
            // NormalW=1 resolves through the palette to node 0; BoneIndices[0]=0 would select
            // the parented node and produce a different result.
            Near(new StaticMapGeometry(source, profile).Bake(mesh, input).Position, new(12, 26, 42), "DS2 rigid selector changed");
        }
        var effectInput=Vertex(0);
        var effect=new StaticMapGeometry(source,"DS1R").Bake(mesh,effectInput,true);
        Check(effect.Colors.Count==2 && effect.Colors.All(c=>c.A==.5f && c.R==.1f && c.G==.2f && c.B==.3f), "Additive effect lost source color/alpha in native color semantic 1");
        Check(effectInput.Colors.Count==1 && effectInput.Colors[0].A==.5f, "Effect baking mutated source alpha");
        mesh.UseBoneWeights = true;
        var weighted = Vertex(99);
        Near(new StaticMapGeometry(source, "DS1R").Bake(mesh, weighted).Position, weighted.Position, "Weighted bind-pose position was transformed again");
    }

    static byte[] CompressedNormal(string fourCC) {
        var header = new DDS { dwWidth = 4, dwHeight = 4, dwMipMapCount = 1, dwPitchOrLinearSize = 16 };
        header.dwFlags |= DDS.DDSD.LINEARSIZE;
        header.ddspf.dwFlags = DDS.DDPF.FOURCC;
        header.ddspf.dwFourCC = fourCC;
        // Valid constant BC3 block with a distinct alpha normal-X channel;
        // ATI2 uses the same sixteen-byte payload budget for a BC5 block.
        byte[] block = [64, 64, 0, 0, 0, 0, 0, 0, 17, 252, 17, 252, 0, 0, 0, 0];
        return header.Write(block);
    }

    static void NormalProfiles() {
        var bc3 = CompressedNormal("DXT5");
        var bc5 = CompressedNormal("ATI2");
        Check((int)new DDS(bc3).GetDXGIFormat() == 77 && (int)new DDS(bc5).GetDXGIFormat() == 83, "Normal fixtures use the wrong pixel formats");
        foreach (var profile in new[] { "DS1", "DS1R" }) {
            Check(MapMaterial.Normal(bc3, profile).SequenceEqual(bc3), profile + " BC3 normal was incorrectly repacked as DS2 DXT5nm");
            Check(MapMaterial.Normal(bc5, profile).SequenceEqual(bc5), profile + " BC5 normal DDS changed");
        }
        foreach (var profile in new[] { "DS2", "DS2S" }) {
            Check(!MapMaterial.Normal(bc3, profile).SequenceEqual(bc3), profile + " BC3 normal was not repacked");
            Check(MapMaterial.Normal(bc5, profile).SequenceEqual(bc5), profile + " BC5 normal DDS changed");
        }
    }

    internal static void Run() {
        RigidPalette();
        OtherSkinModes();
        NormalProfiles();
        Console.WriteLine("PASS: DS1 rigid palettes and parent worlds, scaled normal/tangent frames, bind-pose preservation and profile-specific normal formats");
    }
}
