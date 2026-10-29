// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.Json;

internal static class MapConnectionsTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new Exception("Invalid connection plan accepted");}
    static MapConnections.SourceIdentity Source(string map="m10_02_00_00",string profile="DS1R")=>new(profile,map);
    static void ID(byte[] target,params byte[] values)=>values.CopyTo(target,0);
    static MSB1 First() {
        var msb=new MSB1();
        msb.Parts.Collisions.Add(new(){Name="floor",ModelName="h0000B2",Position=new(1,2,3),Rotation=new(0,15,0),PlayRegionID=1010,DisableStart=true});
        var connection=new MSB1.Part.ConnectCollision{Name="to_burg",ModelName="h0000B2",CollisionName="floor",Position=new(4,5,6)};ID(connection.MapID,10,1,255,255);msb.Parts.ConnectCollisions.Add(connection);
        msb.Parts.Players.Add(new(){Name="start",EntityID=1000999,Position=new(7,8,9)});
        msb.Regions.Regions.Add(new(){Name="arrival",EntityID=1002000,Position=new(10,11,12),Shape=new MSB.Shape.Box{Width=2,Height=3,Depth=4}});
        msb.Events.SpawnPoints.Add(new(){Name="spawn",EventID=31,EntityID=1002001,SpawnPointName="arrival"});
        msb.Events.MapOffsets.Add(new(){Name="origin",Position=new(50,60,70),RotationY=90});return msb;
    }
    static MSB2 Second() {
        var msb=new MSB2();msb.Parts.Collisions.Add(new(){Name="floor2",ModelName="h0000"});
        var connection=new MSB2.Part.ConnectCollision{Name="to_forest",CollisionName="floor2",Position=new(11,12,13)};ID(connection.MapID,10,10,255,255);msb.Parts.ConnectCollisions.Add(connection);
        msb.Parts.WarpCollisions.Add(new(){Name="warp_surface",CollisionName="floor2",Position=new(21,22,23)});
        msb.Regions.StartPoints.Add(new(){Name="arrival2",UniqueID=12,Position=new(31,32,33)});
        msb.Events.Warps.Add(new(){Name="warp2",EventID=42,Position=new(41,42,43),UnkT00=7});
        msb.Events.MapOffsets.Add(new(){Name="origin2",Translation=new(1000,2000,3000)});return msb;
    }
    internal static void Run() {
        var source=Source();var destination=Source("m10_01_00_00");var first=First();
        var inventory=MapConnections.Inspect(first,source,[source,destination,Source("m10_01_00_00","DS3")]);var connection=inventory.Connections.Single();
        Check(inventory.Schema=="bbmap-connections-1"&&source.Key=="DS1R:m10_02_00_00"&&source.Area=="DS1R:m10_02","Source area/profile identity lost");
        Check(connection.Destination.Components.SequenceEqual(new[]{10,1,255,255})&&connection.Destination.Pattern=="m10_01_??_??","Wildcard map ID was normalized or hidden");
        Check(connection.Resolution=="unique"&&connection.Candidates.Single()==destination,"Wildcard resolution mixed source profiles or failed");
        Check(connection.Collision?.Transform.Position==new Vector3(1,2,3)&&connection.Transform.Position==new Vector3(4,5,6),"Connection and source collision transforms were conflated");
        Check(connection.Collision!.Values["PlayRegionID"]==1010&&connection.Collision.Values["DisableStart"]==1,"Collision gameplay gates were omitted from provenance");
        var region=inventory.Regions.Single();Check(region.Dimensions["Height"]==3&&region.EntityID==1002000&&region.References.Single().Field=="SpawnPointName","Region shape/ID/reference provenance omitted");
        Check(inventory.TravelMarkers.Single(m=>m.Kind=="SpawnPoint").Transform?.Position==region.Transform.Position&&inventory.TravelMarkers.Single(m=>m.Kind=="PlayerStart").EntityID==1000999,"Arrival provenance omitted");
        Check(!inventory.MapOffsets.Single().ApplyToPartPositions&&inventory.MapOffsets.Single().RotationY==90,"DS1 map origin was marked for double application");
        Check(inventory.Diagnostics.Length==0&&inventory.Dependencies.Single(d=>d.Kind=="navigation").Paths.Any(p=>p.EndsWith(".mcp")),"Required connection assets missing");
        first.Parts.ConnectCollisions[0].MapID[0]=99;first.Parts.Collisions[0].DrawGroups[0]=0;
        Check(connection.Destination.Components[0]==10&&connection.Collision!.DrawGroups[0]==uint.MaxValue,"Inventory retains mutable source arrays");
        string json=JsonSerializer.Serialize(inventory,new JsonSerializerOptions{IncludeFields=true});
        Check(json.Contains("\"Components\":[10,1,255,255]")&&json.Contains("\"ApplyToPartPositions\":false"),"JSON hides raw map ID components or offset provenance");
        Check(MapConnections.Inspect(First(),source).Connections.Single().Resolution=="catalog-not-supplied","Missing catalog was treated as a missing map");
        Check(MapConnections.Inspect(First(),source,[]).Connections.Single().Resolution=="unavailable","Empty catalog ignored");
        var ambiguous=MapConnections.Inspect(First(),source,[destination,Source("m10_01_00_01")]);Check(ambiguous.Connections.Single().Resolution=="ambiguous","Wildcard map ambiguity silently resolved");
        var missing=First();missing.Parts.Collisions.Clear();Check(MapConnections.Inspect(missing,source,[destination]).Diagnostics.Single().Contains("unavailable collision"),"Unresolved collision reference was discarded");
        var missingRegion=First();missingRegion.Regions.Regions.Clear();Check(MapConnections.Inspect(missingRegion,source,[destination]).Diagnostics.Single().Contains("unavailable region"),"Unresolved arrival reference was discarded");
        Reject(()=>new MapConnections.MapPattern([10,1,254,0]));Reject(()=>new MapConnections.SourceIdentity("DS1R","../map"));
        Reject(()=>MapConnections.Inspect(First(),Source(profile:"DS2S")));
        var nonfinite=First();nonfinite.Parts.ConnectCollisions[0].Position=new(float.NaN,0,0);Reject(()=>MapConnections.Inspect(nonfinite,source));

        var second=Second();var ds2=MapConnections.Inspect(second,Source("m10_04_00_00","DS2S"),[Source("m10_10_00_00","DS2S")]);
        Check(ds2.Connections.Single().Resolution=="unique"&&ds2.Regions.Single().UniqueID==12,"DS2 connection/region identity missing");
        Check(ds2.TravelMarkers.Select(m=>m.Kind).Order().SequenceEqual(new[]{"StartPoint","WarpCollision","WarpEvent"}),"DS2 travel provenance omitted");
        Check(ds2.TravelMarkers.Single(m=>m.Kind=="WarpEvent").Transform!.Position==new Vector3(41,42,43),"DS2 map offset was incorrectly added to source positions");
        Check(ds2.TravelMarkers.Single(m=>m.Kind=="WarpEvent").Values!["UnkT00"]==7,"Opaque DS2 warp byte was discarded");
        Check(!ds2.MapOffsets.Single().ApplyToPartPositions&&ds2.MapOffsets.Single().Translation==new Vector3(1000,2000,3000),"DS2 origin provenance lost");
        Check(ds2.TravelMarkers.Where(m=>m.Kind.StartsWith("Warp")).All(m=>m.UnresolvedSemantics.Length>0),"Unknown DS2 warp semantics presented as converted");

        string[] slots=["m24_01_00_01","m24_02_00_00","m24_03_00_00"];
        var a=new MapConnections.AllocationRequest(source,new(10,20,30),new(100,200,300));
        var b=new MapConnections.AllocationRequest(destination,Vector3.Zero,new(5,6,7));
        var reserved=MapConnections.Reserve([a,b],slots);var reordered=MapConnections.Reserve([b,a],slots.Reverse());
        Check(reserved.Maps.SequenceEqual(reordered.Maps),"Allocation depends on request/slot ordering");
        var appended=MapConnections.Reserve([new(Source("m10_00_00_00"),Vector3.Zero,Vector3.Zero)],slots,reserved.Maps);
        Check(reserved.Maps.All(old=>appended.Maps.Contains(old)),"Appending a source renumbered existing reservations");
        Check(reserved.Maps.Single(r=>r.Source==source).Translation==new Vector3(90,180,270),"Allocation lost explicit coordinate translation");
        Check(reserved.Maps.All(r=>r.AssetNamespace.StartsWith("imp_ds1r_m10_"))&&reserved.SharedOutputs.Single(o=>o.Path.EndsWith("tpfbhd")).Sources.Length==2,"Asset namespace or shared texture composition omitted");
        Reject(()=>MapConnections.Reserve([a,a],slots));Reject(()=>MapConnections.Reserve([a with{TargetMap="m25_00_00_00"}],slots));
        Reject(()=>MapConnections.Reserve([a with{TargetMap="m24_01_00_00"},b with{TargetMap="m24_01_01_01"}],["m24_01_00_00","m24_01_01_01"]));
        Reject(()=>MapConnections.Reserve([a with{TargetAnchor=Vector3.One}],slots,reserved.Maps));
        var crossProfile=MapConnections.Reserve([a,new(Source(source.Map,"DS3"),Vector3.Zero,Vector3.Zero)],slots);
        Check(crossProfile.Maps.Select(m=>m.AssetNamespace).Distinct().Count()==2,"Source games share an asset namespace");
        var explicitChoice=MapConnections.Reserve([a,b with{TargetMap=slots[0]}],slots);
        Check(explicitChoice.Maps.Single(m=>m.Source==destination).TargetMap==slots[0],"Automatic allocation stole an explicit reservation");
        var back=First();ID(back.Parts.ConnectCollisions[0].MapID,10,2,255,255);back.Parts.ConnectCollisions[0].Name="back";
        var destInventory=MapConnections.Inspect(back,destination,[source,destination]);
        var plan=MapConnections.Plan([inventory,destInventory],reserved,new Dictionary<string,string>{{MapConnections.CollisionKey(source,"floor"),"hImportedFloor"}});
        var proposal=plan.Links.Single(l=>l.Source==source);
        Check(proposal.Status=="planned-needs-export-and-runtime-validation"&&proposal.ReciprocalConnections.Single()=="back"&&proposal.NativeCollision=="hImportedFloor","Qualified native collision or reciprocal provenance missing");
        Check(plan.Links.All(l=>!l.RuntimeEnabled)&&proposal.TranslationDifference!=null&&proposal.RemainingWork.Length>0,"Planning claimed activated or proven map traversal");
        Check(plan.Links.Single(l=>l.Source==destination).Status=="native-collision-unmapped","Unmapped collision hidden");
        Check(MapConnections.Plan([ambiguous],reserved).Links.Single().Destination==null,"Ambiguous destination became a native link");
        Console.WriteLine("PASS: DS1/DS2 connection and arrival provenance, wildcard resolution, source/profile identity, stable explicit native reservations, shared assets, unresolved warp semantics and unactivated link plans");
    }
}
