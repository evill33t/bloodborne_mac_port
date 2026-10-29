// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

static class MapReplacementTests {
    static void Check(bool value, string why) { if (!value) throw new Exception(why); }
    internal static void Run() {
        Check(MapMaterial.SameSlot("g_Diffuse","g_DiffuseTexture") && MapMaterial.SameSlot("g_Bumpmap","g_BumpmapTexture"), "DS1 primary texture slots are not mapped");
        Check(!MapMaterial.SameSlot("g_Diffuse_2","g_DiffuseTexture"), "Secondary texture layer replaced the primary layer");
        foreach(var profile in new[]{"DS1","DS1R"}) {
            Check(MapMaterial.Cutout("M_2Foliage[DSB].mtd",profile) && MapMaterial.Cutout("M_3Ivy[DSB]_Edge.mtd",profile) && MapMaterial.Cutout("M[D]_Edge.mtd",profile), "DS1 leaf cards must use cutout materials and skip solid collision");
            Check(!MapMaterial.Cutout("M_2Tree[DSB].mtd",profile) && !MapMaterial.Cutout("M[DB].mtd",profile), "Opaque trunks/stone must retain solid collision");
        }
        Check(MapMaterial.NonSolidFoliage("M[D]_Edge.mtd","m10_grass_10.tga","DS1R") && MapMaterial.NonSolidFoliage("M[DB]_Edge.mtd","o1470_m10_ivy_01.tga","DS1R"), "Generic cutout foliage remains solid");
        Check(!MapMaterial.NonSolidFoliage("P_DullLeather[DSB]_Edge.mtd","o0110.tga","DS1R") && !MapMaterial.NonSolidFoliage("M[DB]_Edge.mtd","m10_obj_chain.tga","DS1R"), "Cutout prop collision was removed");
        Check(!MapMaterial.Cutout("M_2Foliage[DSB].mtd","DS2S"), "DS1 cutout policy changed DS2 materials");
        Check(MapMaterial.LightShaft(@"N:\FRPG\mtd\A10_BG_shaft[Dn]_Add.mtd","DS1R"), "Additive shaft was not classified separately from opaque geometry/collision");
        Check(!MapMaterial.LightShaft("M_1Wood[DSB]_Edge.mtd","DS1R") && !MapMaterial.LightShaft("A10_BG_shaft[Dn]_Add.mtd","DS2S"), "Light-shaft policy classified ordinary geometry or changed DS2");
        var sky = new MSBB();
        foreach(var model in new[]{"m900000","m900010","m900020","m900030","m900090","m900091","m909001","m000010"}) {
            sky.Models.MapPieces.Add(new MSBB.Model.MapPiece(model));
            sky.Parts.MapPieces.Add(new MSBB.Part.MapPiece{Name=model+"_0000",ModelName=model});
        }
        Check(MapReplacement.SelectSky(sky,"m900000")==6, "Overlapping sky phases and distant city scenery remain");
        var selected = MSBB.Read(sky.Write());
        Check(selected.Parts.MapPieces.Select(p=>p.ModelName).SequenceEqual(new[]{"m900000","m000010"}) && selected.Models.MapPieces.Count==2, "Sky selection removed ordinary geometry or retained stale models");
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
        var genericSky = map.Parts.MapPieces.Single(p => p.Name == "sky");
        Check(MapReplacement.SelectSky(map, "m900000") == 0 && map.Parts.MapPieces.Count == 5 && ReferenceEquals(genericSky, map.Parts.MapPieces.Single(p => p.Name == "sky")), "Unaddressed selected sky should remain without an extra clone");
        map.Models.Enemies.Add(new MSBB.Model.Enemy("c1000"));
        map.Parts.Enemies.Add(new MSBB.Part.Enemy { Name = "oldActor", ModelName = "c1000", EntityID = 2410200, NPCParamID = 1000 });
        map.Events.Generators.Add(new MSBB.Event.Generator { Name = "spawner" });
        map.Events.Generators[0].SpawnPartNames[0] = "oldActor";
        Check(MapReplacement.ParkActors(map) == 1 && map.Parts.Enemies.Count == 0, "Old actors remain active");
        reopened = MSBB.Read(map.Write());
        Check(reopened.Parts.DummyEnemies[0].EntityID == 2410200 && reopened.Parts.DummyEnemies[0].NPCParamID == 1000 && reopened.Events.Generators[0].SpawnPartNames[0] == "oldActor", "Parked actor metadata/references changed");

        var skyMap = new MSBB();
        MSBB.Part.MapPiece SkyPiece(string name, string model, int entity = -1) {
            skyMap.Models.MapPieces.Add(new MSBB.Model.MapPiece(model));
            var piece = new MSBB.Part.MapPiece { Name = name, ModelName = model, EntityID = entity, Scale = System.Numerics.Vector3.One };
            Array.Fill(piece.DrawGroups, uint.MaxValue);
            Array.Fill(piece.BackreadGroups, uint.MaxValue);
            Array.Fill(piece.DispGroups, uint.MaxValue);
            skyMap.Parts.MapPieces.Add(piece);
            return piece;
        }
        var day = SkyPiece("day", "m900000", 2414010);
        SkyPiece("scriptNight", "m900020", 2414012);
        SkyPiece("eventEvening", "m900010");
        SkyPiece("objectRedMoon", "m900030");
        SkyPiece("platoonSky", "m900050");
        SkyPiece("city", "m900090");
        SkyPiece("dummy", "m909001");
        SkyPiece("ordinary", "m000010");
        SkyPiece("day_import_day", "m000011").InstanceID = 42;
        skyMap.Events.SFX.Add(new MSBB.Event.SFX { Name = "skyFx", PartName = "eventEvening" });
        skyMap.Events.PlatoonInfo.Add(new MSBB.Event.PlatoonInfo { Name = "skyGroup" });
        skyMap.Events.PlatoonInfo[0].GroupPartsNames[0] = "platoonSky";
        skyMap.Models.Objects.Add(new MSBB.Model.Object("o000010"));
        skyMap.Parts.Objects.Add(new MSBB.Part.Object { Name = "skyOwner", ModelName = "o000010", CollisionName = "objectRedMoon" });
        Check(MapReplacement.SelectSky(skyMap, "m900000") == 2, "Sky selection must count only deleted unreferenced parts");
        Check(skyMap.Parts.MapPieces.Count == 8 && skyMap.Parts.MapPieces.Any(p => p.Name == "ordinary"), "Sky selection removed ordinary geometry or preserved records");
        var dayVisual = skyMap.Parts.MapPieces.Single(p => p.ModelName == "m900000" && p.EntityID == -1);
        Check(dayVisual.Name == "day_import_day_1" && dayVisual.InstanceID == 43, "Independent sky name or instance ID is not unique");
        Check(dayVisual.Scale == System.Numerics.Vector3.One && dayVisual.DrawGroups.All(g => g == uint.MaxValue) && dayVisual.BackreadGroups.All(g => g == uint.MaxValue) && dayVisual.DispGroups.All(g => g == 0), "Independent sky has incorrect geometry or visibility");
        Check(day.EntityID == 2414010, "Original selected sky lost its script identity");
        foreach (var part in skyMap.Parts.MapPieces.Where(p => p.ModelName.StartsWith("m9") && p != dayVisual))
            Check(part.Scale == System.Numerics.Vector3.Zero && part.DrawGroups.All(g => g == 0) && part.BackreadGroups.All(g => g == 0) && part.DispGroups.All(g => g == 0), "Preserved alternate sky remains visible");
        var skyRoundtrip = MSBB.Read(skyMap.Write());
        Check(skyRoundtrip.Parts.MapPieces.Single(p => p.Name == "scriptNight").EntityID == 2414012 && skyRoundtrip.Parts.MapPieces.Single(p => p.Name == "day").EntityID == 2414010, "Script-addressable sky lost its identity");
        Check(skyRoundtrip.Parts.MapPieces.Single(p => p.Name == "day_import_day_1").EntityID == -1 && skyRoundtrip.Parts.MapPieces.Select(p => p.Name).Distinct().Count() == skyRoundtrip.Parts.MapPieces.Count, "Independent sky roundtrip changed identity or duplicated a name");
        Check(skyRoundtrip.Events.SFX.Single().PartName == "eventEvening" && skyRoundtrip.Parts.Objects.Single().CollisionName == "objectRedMoon" && skyRoundtrip.Events.PlatoonInfo.Single().GroupPartsNames[0] == "platoonSky", "Sky selection broke native part references");
        Check(skyRoundtrip.Models.MapPieces.All(m => m.Name is not ("m900090" or "m909001")) && skyRoundtrip.Parts.MapPieces.All(p => skyRoundtrip.Models.MapPieces.Any(m => m.Name == p.ModelName)), "Sky model registration is inconsistent");

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
