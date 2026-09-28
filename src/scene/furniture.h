// Furniture of the game: the chess table and the players' chairs (procedural, Louis XVI style).
//
//   Model table = buildTable();   // world space, centred on the origin
//   Model chair = buildChair();   // chair-local space, place with a model matrix
//
// Table: flat waxed top exactly at layout::TABLE_TOP_Y (TABLE_WIDTH along X x TABLE_DEPTH along Z,
// TABLE_TOP_THICKNESS thick) in TableWood with a moulded edge; carved apron and four fluted
// tapering legs in TableWoodCarved with small gilded rosettes. The whole volume under the table
// between the legs is free for the seated robots' legs: the aprons on the players' sides (+-Z)
// are only TABLE_KNEE_APRON_BOTTOM high at their lowest point and set back to |z| = TABLE_APRON_Z,
// the legs stand at the corners (|x| ~ 0.51).
//   * Part "table_top" (TableWood): top face uv = world (x, z) in meters, normal +Y at TABLE_TOP_Y.
//
// Chair: origin on the floor under the seat centre, facing +Z (the sitter looks towards +Z), seat
// cushion top at layout::SEAT_HEIGHT. Armless médaillon chair: fluted legs, moulded seat rails
// and oval back frame (ChairWood), velvet seat and back cushions (ChairVelvet). The back cushion's
// front surface is ~CHAIR_BACK_Z behind the seat centre. Place it at z = +-layout::CHAIR_Z facing the
// table (White: rotateY(pi) at +CHAIR_Z, Black: identity at -CHAIR_Z).
#pragma once
#include "../game/layout.h"
#include "model.h"

namespace furniture {

constexpr float TABLE_APRON_Z = 0.375f;           // |z| of the outer face of the players' side aprons
constexpr float TABLE_KNEE_APRON_BOTTOM = 0.655f; // lowest point of those aprons (knee clearance)
constexpr float TABLE_LEG_X = 0.513f, TABLE_LEG_Z = 0.343f;  // leg axes
constexpr float CHAIR_BACK_Z = 0.17f;             // seat centre -> front of the back cushion (along -Z)

Model buildTable();
Model buildChair();

}  // namespace furniture
