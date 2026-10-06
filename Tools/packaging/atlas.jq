# Shared JSON-only AppImage startup layout. Robot views precede guest views.
def integer($lo;$hi): type == "number" and floor == . and . >= $lo and . <= $hi;
if (.robots | type) != "array" then error("robots must be an array") else . end |
(.vision.mode // "stereo_rgb") as $mode |
if ($mode != "stereo_rgb" and $mode != "rgbd") then error("vision.mode must be stereo_rgb or rgbd") else . end |
((.robots|length) * (if $mode == "rgbd" then 1 else 2 end)) as $robots |
(.guest_inspector // {}) as $guest |
(if $guest.enabled == false then 0 else ($guest.max_guests // 4) end) as $guests |
($guest.width // 640) as $gw | ($guest.height // 480) as $gh |
if (($guests|integer(0;4)) and ($gw|integer(64;1920)) and ($gh|integer(64;1080)) and $gw%2 == 0 and $gh%2 == 0) | not
then error("invalid guest count or resolution") else . end |
($robots + $guests) as $total |
if $total == 0 then error("scene requires at least one robot or guest view") else . end |
([640,$gw]|max) as $cw | ([480,$gh]|max) as $ch |
(($total*4/3)|sqrt|ceil) as $cols | (($total/$cols)|ceil) as $rows |
{nDisplay:{description:"URS camera atlas",version:"5.00",assetPath:"",
 misc:{bFollowLocalPlayerCamera:false,bExitOnEsc:true,bOverrideViewportsFromExternalConfig:true,bOverrideTransformsFromExternalConfig:true},
 scene:{xforms:{},cameras:{DefaultViewPoint:{interpupillaryDistance:6.4,swapEyes:false,stereoOffset:"none",parentId:"",location:{x:0,y:0,z:0},rotation:{pitch:0,yaw:0,roll:0}}},screens:{}},
 cluster:{primaryNode:{id:"node_0",ports:{ClusterSync:41001,ClusterEventsJson:41003,ClusterEventsBinary:41004}},
 sync:{renderSyncPolicy:{type:"none",parameters:{}},inputSyncPolicy:{type:"ReplicatePrimary",parameters:{}}},
 network:{ConnectRetriesAmount:"10",ConnectRetryDelay:"100",GameStartBarrierTimeout:"30000",FrameStartBarrierTimeout:"30000",FrameEndBarrierTimeout:"30000",RenderSyncBarrierTimeout:"30000"},
 nodes:{node_0:{host:"127.0.0.1",sound:false,fullScreen:false,window:{x:0,y:0,w:($cols*$cw),h:($rows*$ch)},postprocess:{},
 viewports:([range(0;$total) as $i |
 {key:((if $i < $robots then "camera_" else "guest_" end) + ((if $i < $robots then $i else $i-$robots end)|tostring|if length < 2 then "0"+. else . end)),
 value:{camera:"DefaultViewPoint",bufferRatio:1,gPUIndex:-1,allowCrossGPUTransfer:false,isShared:false,
 region:{x:(($i%$cols)*$cw),y:(($i/$cols|floor)*$ch),w:(if $i<$robots then 640 else $gw end),h:(if $i<$robots then 480 else $gh end)},projectionPolicy:{type:"camera",parameters:{}}}}]|from_entries),
 outputRemap:{bEnable:false,dataSource:"mesh",staticMeshAsset:"",externalFile:""}}}},
 customParameters:{},diagnostics:{simulateLag:false,minLagTime:0.01,maxLagTime:0.3}}}
