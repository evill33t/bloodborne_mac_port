// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

static class MapReplacementTests {
    static void Check(bool value, string why) { if (!value) throw new Exception(why); }
    internal static void Run() {
        var map = new MSBB();
        void Piece(string name, string model, int entity = -1) {
            map.Models.MapPieces.Add(new MSBB.Model.MapPiece(model));
            map.Parts.MapPieces.Add(new MSBB.Part.MapPiece { Name = name, ModelName = model, EntityID = entity });
        }
        Piece("unused", "m000001"); Piece("sky", "m900000");
        Piece("event", "m000002"); Piece("objectParent", "m000003");
        Piece("script", "m000004", 2411000); Piece("pickup", "m000005");
        map.Events.SFX.Add(new MSBB.Event.SFX { Name = "fx", PartName = "event" });
        map.Events.Treasures.Add(new MSBB.Event.Treasure { Name = "item", TreasurePartName = "pickup" });
        map.Models.Objects.Add(new MSBB.Model.Object("o000001"));
        map.Parts.Objects.Add(new MSBB.Part.Object { Name = "obj", ModelName = "o000001", CollisionName = "objectParent" });
        Check(MapReplacement.PruneScenery(map) == 1, "Pruning must retain sky, script IDs, events and object loading references");
        Check(map.Models.MapPieces.All(m => m.Name != "m000001"), "Unused scenery model retained");
        var reopened = MSBB.Read(map.Write());
        Check(reopened.Parts.MapPieces.Count == 5 && reopened.Parts.Objects[0].CollisionName == "objectParent", "Pruned map references failed a round trip");
        map.Models.Enemies.Add(new MSBB.Model.Enemy("c1000"));
        map.Parts.Enemies.Add(new MSBB.Part.Enemy { Name = "oldActor", ModelName = "c1000", EntityID = 2410200, NPCParamID = 1000 });
        map.Events.Generators.Add(new MSBB.Event.Generator { Name = "spawner" });
        map.Events.Generators[0].SpawnPartNames[0] = "oldActor";
        Check(MapReplacement.ParkActors(map) == 1 && map.Parts.Enemies.Count == 0, "Old actors remain active");
        reopened = MSBB.Read(map.Write());
        Check(reopened.Parts.DummyEnemies[0].EntityID == 2410200 && reopened.Parts.DummyEnemies[0].NPCParamID == 1000 && reopened.Events.Generators[0].SpawnPartNames[0] == "oldActor", "Parked actor metadata/references changed");

        // Constant DXT5nm block: normal X=64 is in alpha; RGB red=255.
        // Include a smaller mip to catch Pfim's padded rows and mip offsets.
        var header = new DDS { dwWidth = 4, dwHeight = 4, dwMipMapCount = 2, dwPitchOrLinearSize = 16 };
        header.dwFlags |= DDS.DDSD.LINEARSIZE | DDS.DDSD.MIPMAPCOUNT;
        header.dwCaps |= DDS.DDSCAPS.COMPLEX | DDS.DDSCAPS.MIPMAP;
        header.ddspf.dwFlags = DDS.DDPF.FOURCC; header.ddspf.dwFourCC = "DXT5";
        byte[] block = [64, 64, 0, 0, 0, 0, 0, 0, 17, 252, 17, 252, 0, 0, 0, 0];
        var normal = MapMaterial.Normal(header.Write(block.Concat(block).ToArray()));
        var buffers = Headerizer.GetDDSTextureBuffers(new DDS(normal), normal);
        Check(buffers.Count == 1 && buffers[0].subImages.Count == 2, "Normal conversion lost mip levels");
        Check(buffers[0].subImages[0].Length == 64 && buffers[0].subImages[1].Length == 16, "Normal mip dimensions changed");
        foreach (var mip in buffers[0].subImages)
            for (int i = 0; i < mip.Length; i += 4)
                Check(mip[i] == 64 && mip[i + 1] is >= 128 and <= 132 && mip[i + 3] == 255, "DS2 alpha/green normal channels were not repacked");
        Console.WriteLine("PASS: scenery pruning preserves references; DXT5nm conversion preserves normal channels and mip levels");
    }
}
