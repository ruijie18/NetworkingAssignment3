/*
 *****************************************************************
 *  File:      client.cpp
 *  Author:    Joshua Sim Yue Chen
 *  Brief:     Implements the client for the asteroid shooter.
 *             The client computes its own ship movement using
 *             simplified physics (client-side prediction) and
 *             sends PLAYER_UPDATE packets (including velocity).
 *             It also receives GAME_UPDATE packets containing
 *             all game objects. We now separate local objects
 *             (local player, bullets, etc.) from remote objects.
 *****************************************************************/

#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include "AEEngine.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <conio.h>
#include <vector>
#include <string>
#include <cmath>
#include <mutex>
#include <map>
#include <algorithm>
#include <string>
#include <set>

#pragma comment(lib, "ws2_32.lib")

 // Constants
constexpr uint16_t SERVER_PORT = 9000;
constexpr int CLIENT_PORT_START = 9001;
constexpr int BUFFER_SIZE = 4096;
constexpr int MAX_REMOTE_OBJECTS = 512;  // Objects coming from the server. // Anything above 256 is considered as cached or fake entities that server does not know, hence the mismatch
constexpr int MAX_LOCAL_ENTITIES = 500;     // Local pool for bullets, power-ups, etc.
constexpr int MAX_LOCAL_ENTITIES_SPAWN_RATE = 10;
constexpr int MAX_PLAYERS = 4000; // Maximum number for score-count
constexpr int VICTORY_SCORE = 500; // Client based intended

#pragma region Helper Func
// ----------------------------------------------------------------------
// Helper: Linear Interpolation Function
// ----------------------------------------------------------------------
inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// ----------------------------------------------------------------------
// Helper: Collision Check (Sphere to sphere) function
// ----------------------------------------------------------------------
bool checkSphereCollision(float x1, float y1, float r1, float x2, float y2, float r2)
{
    return (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) <= (r1 + r2) * (r1 + r2);
}
#pragma endregion

#pragma region Packets Declaration
// Packet types (must match server definitions)
enum PacketType : uint8_t
{
    JOIN_REQUEST = 0x01,
    JOIN_ACCEPT = 0x02,
    GAME_UPDATE = 0x03,
    PLAYER_UPDATE = 0x04,
    ACK = 0x05,
    BULLET_SPAWN = 0x06,  // New packet type for bullet spawning.
    // Score packets
    DISCONNECT = 0x07,
    NAME_REJECTED = 0x08,  // new packet type
    SCORE_INCREMENT = 0x09,
    SCORE_UPDATE = 0x10,
    FINAL_SCOREBOARD = 0x11,
    ASTEROID_HIT = 0x12
};

#pragma pack(push, 1)

// Packet for join request.
struct JoinRequestPacket {
    uint8_t type = JOIN_REQUEST;
    char name[32];  // Max 31 characters + null terminator
};

// Packet for join acceptance.
struct JoinAcceptPacket {
    uint8_t type = JOIN_ACCEPT;
    uint32_t playerId;
};

// PLAYER_UPDATE packet: now includes velocity.
struct PlayerUpdatePacket {
    uint8_t type = PLAYER_UPDATE;
    uint32_t playerId;
    float pos_x;
    float pos_y;
    float angle;   // Orientation in radians.
    float vel_x;   // Linear velocity X.
    float vel_y;   // Linear velocity Y.
};

// Bullet spawn packet: sent once when the client spawns a bullet.
struct BulletSpawnPacket {
    uint8_t type = BULLET_SPAWN;
    uint32_t playerId; // Owner's ID.
    uint8_t bulletType; // Bullet type identifier.
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    float damage;
};

struct BulletSpawnMultiPacket {
    uint8_t type = BULLET_SPAWN;   // This will be BULLET_SPAWN or a new type if you prefer.
    uint32_t count;       // Number of bullets being sent.
    // Then an array of bullet data. For simplicity, we assume a fixed maximum.
    BulletSpawnPacket bullets[MAX_LOCAL_ENTITIES_SPAWN_RATE];  // Adjust size as necessary.
};

// Game object data received from the server (for remote objects).
struct GameObjectData {
    uint8_t objectType; // Use ObjectType
    uint32_t playerId;  // For player objects; for others, can be 0.
    float pos_x;
    float pos_y;
    float rotation;
    float scale;
    float vel_x;      // Velocity X component.
    float vel_y;      // Velocity Y component.
    uint32_t entityId;
    bool isActive = false;
};

struct GameUpdatePacket {
    uint8_t type = GAME_UPDATE;
    uint32_t objectCount;
    GameObjectData objects[MAX_REMOTE_OBJECTS];
};

struct ScoreIncrementPacket {
    uint8_t type = SCORE_INCREMENT;
    uint32_t playerId;
    uint32_t increment;
    uint32_t entityId;
};

struct PlayerScore {
    uint32_t playerId;
    uint32_t score;
    char name[32];  // Add this field
};

struct ScoreUpdatePacket {
    uint8_t type = SCORE_UPDATE;
    uint32_t scoreCount;
    PlayerScore scores[MAX_PLAYERS];
};

struct AsteroidUpdatePacket {
    uint8_t type = ASTEROID_HIT;
    uint32_t entityId;
};

#pragma pack(pop)
#pragma endregion

#pragma region VARIABLES DECLARATION

// ----------------------------------------------------------------------
// Global Networking Variables
// ----------------------------------------------------------------------
std::atomic<bool> running{ true };
std::atomic<bool> gGameRunning{ true };
uint32_t myPlayerId = 0;
SOCKET udpSocket = INVALID_SOCKET;
sockaddr_in serverAddr{};

// Score variables
std::map<uint32_t, uint32_t> gScoreBoard;
std::mutex gScoreMutex;
std::map<uint32_t, std::string> gPlayerNames;

// Game State 
enum GameState {
    Playing,
    Pause,
    Win
};

GameState gameState = Playing; // Defaults allows simulation, etc


// Animation Client Variables
float scoreboardAnimTimer = 0.0f;
const float scoreboardAnimDuration = 1.5f; // seconds

// Destroyed Asteroids
std::set<uint32_t> destroyedEntityIds;
std::mutex destroyedMutex;

// ----------------------------------------------------------------------
// Entity Structures & Pools
// ----------------------------------------------------------------------
struct GameObject
{
    AEMtx33 transform;  // For rendering.
    float pos_x = 0.0f, pos_y = 0.0f;
    float vel_x = 0.0f, vel_y = 0.0f;
    float scale = 1.0f;
    float rotation = 0.0f;
    uint8_t objectType = 0; // Use Object Type
    uint32_t playerId = 0;  // Valid for player objects.
    uint32_t entityId = 0;
    bool isSent = false;
    bool isActive = false;
};

enum ObjectType
{
    Player,
    Asteroid,
    Bullet
};

// Remote pool: all entities updated from the server.
GameObject gRemoteEntities[MAX_REMOTE_OBJECTS]; // Pool for entities from server pool to render & utilize on client
GameObject gServerEntityPool[MAX_REMOTE_OBJECTS]; // Pool for entities from server directly from recv messages

// Local pool: stores local player and other local spawned objects.
GameObject gLocalPlayer; // Local player object.
GameObject gLocalEntities[MAX_LOCAL_ENTITIES];

// Mutex for synchronizing access to both pools.
std::mutex gPoolMutex;

// ----------------------------------------------------------------------
// Local Player Physics Variables (Client-Side Prediction)
// ----------------------------------------------------------------------
float playerPosX = 400.0f;
float playerPosY = 300.0f;
float playerAngle = 0.0f;           // Orientation in radians.
float playerVelX = 0.0f;
float playerVelY = 0.0f;
float playerAngularVelocity = 0.0f;
const float playerRenderScale = 100.0f;  // For rendering scale.

// AE-Engine mesh and textures
AEGfxVertexList* pMesh = nullptr;
AEGfxTexture* Background1Texture = nullptr;
AEGfxTexture* AsteroidTexture = nullptr;
AEGfxTexture* PlanetTexture = nullptr;
AEGfxTexture* Player1Texture = nullptr;
AEGfxTexture* Player2Texture = nullptr;
AEGfxTexture* Player3Texture = nullptr;
AEGfxTexture* Player4Texture = nullptr;
AEGfxTexture* BulletTexture = nullptr;
s8	pFont;

#pragma endregion

#pragma region Local Spawning
// ----------------------------------------------------------------------
// SpawnLocalEntity: Spawns a new local entity (e.g. bullet, power-up).
// ----------------------------------------------------------------------
void SpawnLocalEntity(uint8_t objectType, float pos_x, float pos_y, float vel_x, float vel_y, float rotation, float scale)
{
    std::lock_guard<std::mutex> lock(gPoolMutex);

    // Find an inactive object in the pool
    for (uint32_t i = 0; i < MAX_LOCAL_ENTITIES; ++i)
    {
        if (!gLocalEntities[i].isActive)  // Reuse an inactive slot
        {
            GameObject& obj = gLocalEntities[i];
            obj.objectType = objectType;
            obj.playerId = myPlayerId;
            obj.pos_x = pos_x;
            obj.pos_y = pos_y;
            obj.vel_x = vel_x;
            obj.vel_y = vel_y;
            obj.rotation = rotation;
            obj.scale = scale;
            obj.isActive = true;  // Mark as active
            return;
        }
    }
}

// ----------------------------------------------------------------------
// SpawnBullet: Sends a bullet spawn packet and spawns the bullet locally.
// ----------------------------------------------------------------------
void SpawnBullet()
{
    // Prepare packet data.
    BulletSpawnPacket pkt{};
    pkt.playerId = myPlayerId;
    pkt.bulletType = 0;   // Standard bullet.
    pkt.pos_x = playerPosX;
    pkt.pos_y = playerPosY;
    float bulletSpeed = 500.0f;
    pkt.vel_x = cosf(playerAngle) * bulletSpeed;
    pkt.vel_y = sinf(playerAngle) * bulletSpeed;
    pkt.damage = 10.0f;

    // Send BULLET_SPAWN packet to server.
    int sentBytes = sendto(udpSocket, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
        reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sentBytes == SOCKET_ERROR) {
        std::cerr << "[ERROR] sendto BULLET_SPAWN failed: " << WSAGetLastError() << std::endl;
    }

    // Spawn bullet locally in the local pool.
    SpawnLocalEntity(2, pkt.pos_x, pkt.pos_y, pkt.vel_x, pkt.vel_y, playerAngle, 20.0f);
}

#pragma endregion

#pragma region Score Logic
// ----------------------------------------------------------------------
//  Score Increment Function
// ----------------------------------------------------------------------
void ReportScoreUpdate(uint32_t playerId, uint32_t points)
{
    ScoreIncrementPacket pkt;
    pkt.type = SCORE_INCREMENT;
    pkt.playerId = playerId;
    pkt.increment = points;

    int sent = sendto(udpSocket, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
        reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sent == SOCKET_ERROR) {
        std::cerr << "[ERROR] sendto SCORE_INCREMENT failed: " << WSAGetLastError() << std::endl;
    }
}

// ----------------------------------------------------------------------
//  Scoreboard render function
// ----------------------------------------------------------------------

void RenderScoreboardText(int Location = 0)
{
    // Lock and copy the current scoreboard
    std::vector<std::pair<uint32_t, uint32_t>> sortedScores;
    std::map<uint32_t, std::string> localPlayerNames;
    {
        std::lock_guard<std::mutex> lock(gScoreMutex);
        sortedScores.assign(gScoreBoard.begin(), gScoreBoard.end());
        localPlayerNames = gPlayerNames; // Copy player names
    }

    // Sort by score (descending)
    std::sort(sortedScores.begin(), sortedScores.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    // Header
    const char* headerText = (Location == 1) ? "Final Scoreboard" : "Scoreboard";

    f32 w = 1.0f, h = 1.0f;
    AEGfxGetPrintSize(pFont, headerText, 0.5f, &w, &h);

    float startY = 1.2f; // Start off-screen above
    float endY = 0.1f;   // Final resting Y center offset

    float animT = scoreboardAnimTimer / scoreboardAnimDuration;
    if (animT > 1.0f) animT = 1.0f;

    float centerYOffset = Lerp(startY, endY, animT);

    float baseX = (Location == 1) ? -w / 2.0f : 1.0f - w - 0.05f;
    float baseY = (Location == 1) ? centerYOffset : 1.0f - h - 0.05f;

    AEGfxPrint(pFont, headerText, baseX, baseY, 0.5f, 1, 1, 1, 1);

    // Render top N players
    const int maxEntries = 6;
    int entryCount = static_cast<int>(sortedScores.size());
    for (int i = 0; i < ((maxEntries < entryCount) ? maxEntries : entryCount); ++i)
    {
        const auto& entry = sortedScores[i];
        uint32_t playerId = entry.first;
        uint32_t score = entry.second;

        // Look up player name
        std::string displayName = "P" + std::to_string(playerId);
        auto it = localPlayerNames.find(playerId);
        if (it != localPlayerNames.end())
            displayName = it->second;

        std::string line = displayName + " : " + std::to_string(score);

        const char* txt = line.c_str();
        f32 lineW = 1.0f, lineH = 1.0f;
        AEGfxGetPrintSize(pFont, txt, 0.5f, &lineW, &lineH);

        float textX = (Location == 1) ? -lineW / 2.0f : 1.0f - lineW - 0.05f;
        float textY = baseY - ((i + 1) * 0.07f);

        // Set color: gold for winner, white for others
        if (i == 0)
            AEGfxPrint(pFont, txt, textX, textY, 0.5f, 1.0f, 0.84f, 0.0f, 1);  // Gold
        else
            AEGfxPrint(pFont, txt, textX, textY, 0.5f, 1, 1, 1, 1);
    }
}

#pragma endregion

#pragma region Asteroid Logic

void ReportAsteroidDestruction(uint32_t entityId)
{
    AsteroidUpdatePacket pkt;
    pkt.type = ASTEROID_HIT;
    pkt.entityId = entityId;

    int sent = sendto(udpSocket, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
        reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sent == SOCKET_ERROR) {
        std::cerr << "[ERROR] sendto ASTEROID_HIT failed: " << WSAGetLastError() << std::endl;
    }
}

#pragma endregion

#pragma region Cleaning/Destroying Objects
//void CleanupLocalEntities()
//{
//    std::lock_guard<std::mutex> lock(gPoolMutex);
//    uint32_t i = 0;
//    while (i < gLocalEntityCount.load())
//    {
//        if (!gLocalEntities[i].isActive)
//        {
//            // Swap with the last active element.
//            gLocalEntities[i] = gLocalEntities[gLocalEntityCount - 1];
//            gLocalEntityCount--;
//            // Do not increment i; process the swapped element.
//        }
//        else
//        {
//            ++i;
//        }
//    }
//}
#pragma endregion

#pragma region Update Logic
// ----------------------------------------------------------------------
// UpdateLocalSimulation: Updates local simulation (player and local entities).
// ----------------------------------------------------------------------
void UpdateLocalSimulation(float dt)
{
    // Process input for player movement.
    float thrustInput = 0.0f;
    float rotationInput = 0.0f;
    if (AEInputCheckCurr(AEVK_W))
        thrustInput = 1.0f;
    if (AEInputCheckCurr(AEVK_S))
        thrustInput = -1.0f;
    if (AEInputCheckCurr(AEVK_D))
        rotationInput = -1.0f;
    if (AEInputCheckCurr(AEVK_A))
        rotationInput = 1.0f;

    const float thrustForce = 150.0f;
    const float torqueForce = 7.0f;
    const float linearDamping = 0.7f;
    const float angularDamping = 0.95f;

    float accel = thrustForce * thrustInput;
    playerVelX += cosf(playerAngle) * accel * dt;
    playerVelY += sinf(playerAngle) * accel * dt;
    playerVelX *= (1.0f - linearDamping * dt);
    playerVelY *= (1.0f - linearDamping * dt);
    playerPosX += playerVelX * dt;
    playerPosY += playerVelY * dt;

    playerAngularVelocity *= (1.0f - angularDamping * dt);
    playerAngularVelocity += torqueForce * rotationInput * dt;
    playerAngle += playerAngularVelocity * dt;

    float windowWidth = 1600.0f;
    float windowHeight = 900.0f;
    if (playerPosX < -windowWidth / 2 - playerRenderScale)
        playerPosX += windowWidth + playerRenderScale * 2;
    else if (playerPosX > windowWidth / 2 + playerRenderScale)
        playerPosX -= windowWidth + playerRenderScale * 2;
    if (playerPosY < -windowHeight / 2 - playerRenderScale)
        playerPosY += windowHeight + playerRenderScale * 2;
    else if (playerPosY > windowHeight / 2 + playerRenderScale)
        playerPosY -= windowHeight + playerRenderScale * 2;

    // Update local player.
    {
        std::lock_guard<std::mutex> lock(gPoolMutex);
        gLocalPlayer.pos_x = playerPosX;
        gLocalPlayer.pos_y = playerPosY;
        gLocalPlayer.vel_x = playerVelX;
        gLocalPlayer.vel_y = playerVelY;
        gLocalPlayer.rotation = playerAngle;
        gLocalPlayer.scale = playerRenderScale;
        gLocalPlayer.objectType = ObjectType::Player; // Local player.
        gLocalPlayer.playerId = myPlayerId;
    }

    // Update local entities (e.g., bullets) in the local pool.
    {
        std::lock_guard<std::mutex> lock(gPoolMutex);
        for (uint32_t i = 0; i < MAX_LOCAL_ENTITIES; i++)
        {
            gLocalEntities[i].pos_x += gLocalEntities[i].vel_x * dt;
            gLocalEntities[i].pos_y += gLocalEntities[i].vel_y * dt;
        }
    }

    // Check for bullet spawn input (fire key, e.g., SPACE).
    if (AEInputCheckTriggered(AEVK_SPACE))
    {
        SpawnBullet();
    }

    //Score increment
    if (AEInputCheckTriggered(AEVK_1)) {
        ReportScoreUpdate(myPlayerId, 10);  // +10 points test
    }
    // Here you could add more input-handling for other local-spawnable entities.
}

// ----------------------------------------------------------------------
// Handle Collision Logic
// ----------------------------------------------------------------------
void HandleCollisionChecks()
{
    std::lock_guard<std::mutex> lock(gPoolMutex);

    for (uint32_t i = 0; i < MAX_REMOTE_OBJECTS / 2; i++)
    {
        if (!gRemoteEntities[i].isActive)
            continue;

#pragma region With Player
#if 1
        if (checkSphereCollision(
            gRemoteEntities[i].pos_x, gRemoteEntities[i].pos_y, gRemoteEntities[i].scale * 0.5f,
            gLocalPlayer.pos_x, gLocalPlayer.pos_y, gLocalPlayer.scale * 0.5f))
        {
            // Handle player collision
        }
#endif
#pragma endregion

#pragma region With Master Client Bullets
#if 1
        for (uint32_t j = 0; j < MAX_LOCAL_ENTITIES; j++)
        {
            if (!gLocalEntities[j].isActive)
                continue;

            if (checkSphereCollision(
                gRemoteEntities[i].pos_x, gRemoteEntities[i].pos_y, gRemoteEntities[i].scale * 0.5f,
                gLocalEntities[j].pos_x, gLocalEntities[j].pos_y, gLocalEntities[j].scale * 0.5f))
            {
                if (gServerEntityPool[i].objectType == ObjectType::Asteroid)
                {
                    gLocalEntities[j].isActive = false;

                    uint32_t entityId = gServerEntityPool[i].entityId;

                    std::lock_guard<std::mutex> dlock(destroyedMutex);
                    if (destroyedEntityIds.count(entityId))
                        continue;

                    destroyedEntityIds.insert(entityId);
                    gServerEntityPool[i].isActive = false;
                    gRemoteEntities[i].isActive = false;

                    ReportScoreUpdate(myPlayerId, 10);
                    ReportAsteroidDestruction(entityId);
                    std::cout << "[LOCAL] Asteroid with ID " << entityId << " destroyed by local bullet\n";
                }
            }
        }
#endif
#pragma endregion

#pragma region With Each Other (different objs from server)
#if 1
        //for (uint32_t j = 0; j < i; j++) // Prevent self-collision by iterating only to i.
        //{
        //    if (!gServerEntityPool[j].isActive)
        //        continue;

        //    if (checkSphereCollision(
        //        gServerEntityPool[i].pos_x, gServerEntityPool[i].pos_y, gServerEntityPool[i].scale * 0.5f,
        //        gServerEntityPool[j].pos_x, gServerEntityPool[j].pos_y, gServerEntityPool[j].scale * 0.5f))
        //    {
        //        // Handle server object collision
        //    }
        //}

        for (uint32_t f = MAX_REMOTE_OBJECTS / 2; f < MAX_REMOTE_OBJECTS; f++)
        {
            if (!gRemoteEntities[f].isActive)
                continue;

            if (checkSphereCollision(
                gRemoteEntities[i].pos_x, gRemoteEntities[i].pos_y, gRemoteEntities[i].scale * 0.5f,
                gRemoteEntities[f].pos_x, gRemoteEntities[f].pos_y, gRemoteEntities[f].scale * 0.5f)
                && gRemoteEntities[i].objectType == ObjectType::Asteroid)
            {
                // Handle fake bullet collision
                gRemoteEntities[f].isActive = false;
                gServerEntityPool[f].isActive = false;
            }
        }
#endif
#pragma endregion
    }
}
#pragma endregion

#pragma region Client Interaction
// ----------------------------------------------------------------------
// Receive Thread: Processes JOIN_ACCEPT, GAME_UPDATE packets.
// ----------------------------------------------------------------------
void ReceiveThread(SOCKET socket)
{
    char buffer[BUFFER_SIZE];
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);
    while (running)
    {
        int bytesReceived = recvfrom(socket, buffer, BUFFER_SIZE, 0,
            reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (bytesReceived == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                continue;
            std::cerr << "[ERROR] recvfrom failed: " << err << std::endl;
            continue;
        }
        if (bytesReceived <= 0)
            continue;

        uint8_t packetType = static_cast<uint8_t>(buffer[0]);
        if (packetType == JOIN_ACCEPT)
        {
            JoinAcceptPacket* pkt = reinterpret_cast<JoinAcceptPacket*>(buffer);
            myPlayerId = pkt->playerId;
            std::cout << "[JOINED] Assigned Player ID: " << myPlayerId << std::endl;
        }
        else if (packetType == GAME_UPDATE)
        {
            GameUpdatePacket* update = reinterpret_cast<GameUpdatePacket*>(buffer);
            uint32_t count = update->objectCount;
            // Limit updates to first half of pool.
            if (count > MAX_REMOTE_OBJECTS / 2)
                count = MAX_REMOTE_OBJECTS / 2;

            std::lock_guard<std::mutex> lock(gPoolMutex);
            // Mark first half of remote pool as inactive (for game updates).
            for (uint32_t i = 0; i < MAX_REMOTE_OBJECTS / 2; i++)
            {
                gServerEntityPool[i].isActive = false;
                gRemoteEntities[i].isActive = false;
            }

            // Fill the first half with updated objects.
            uint32_t poolIndex = 0;
            for (uint32_t i = 0; i < count; i++) {
                const GameObjectData& src = update->objects[i];

                // Skip updates from local player & local bullets.
                if ((src.objectType == ObjectType::Player && src.playerId == myPlayerId) ||
                    (src.objectType == ObjectType::Bullet && src.playerId == myPlayerId))
                    continue;

                if (poolIndex >= MAX_REMOTE_OBJECTS / 2)
                    break;

                GameObject& serverObj = gServerEntityPool[poolIndex];
                serverObj.pos_x = src.pos_x;
                serverObj.pos_y = src.pos_y;
                serverObj.rotation = src.rotation;
                serverObj.scale = src.scale;
                serverObj.objectType = src.objectType;
                serverObj.playerId = src.playerId;
                serverObj.vel_x = src.vel_x;
                serverObj.vel_y = src.vel_y;
                serverObj.entityId = src.entityId;
                serverObj.isActive = src.isActive;

                gRemoteEntities[poolIndex] = serverObj;

                poolIndex++;
            }
        }
        else if (packetType == BULLET_SPAWN)
        {
            // The minimum size for a multi-bullet packet includes type + count.
            const size_t minMultiPacketSize = sizeof(uint8_t) + sizeof(uint32_t);
            if (bytesReceived >= minMultiPacketSize)
            {
                // Read the bullet count field.
                uint32_t count = *reinterpret_cast<uint32_t*>(buffer + sizeof(uint8_t));

                // Calculate the expected packet size.
                size_t expectedSize = sizeof(uint8_t) + sizeof(uint32_t) + count * sizeof(BulletSpawnPacket);
                if (bytesReceived >= expectedSize && count >= 1)
                {
                    // Cast the buffer to a multi-bullet packet structure.
                    BulletSpawnMultiPacket* multiPkt = reinterpret_cast<BulletSpawnMultiPacket*>(buffer);
                    // Process each bullet in the multi-packet.
                    for (uint32_t i = 0; i < multiPkt->count; i++)
                    {
                        if (multiPkt->bullets[i].playerId == myPlayerId)
                            break; // Avoid spawning bullets for the master client.

                        // Calculate the bullet’s angle from its velocity.
                        float angle = atan2f(multiPkt->bullets[i].vel_y, multiPkt->bullets[i].vel_x);

                        // Create a new bullet GameObject.
                        GameObject bullet;
                        bullet.objectType = ObjectType::Bullet;          // Bullet type.
                        bullet.playerId = multiPkt->bullets[i].playerId;
                        bullet.pos_x = multiPkt->bullets[i].pos_x;
                        bullet.pos_y = multiPkt->bullets[i].pos_y;
                        bullet.vel_x = multiPkt->bullets[i].vel_x;
                        bullet.vel_y = multiPkt->bullets[i].vel_y;
                        bullet.rotation = angle;
                        bullet.scale = 100.0f;          // Adjust scale as desired.
                        bullet.isActive = true;         // Mark bullet as active.

                        // Search for an inactive bullet slot in the designated bullet region (second half).
                        int bulletIndex = -1;
                        for (int j = MAX_REMOTE_OBJECTS / 2; j < MAX_REMOTE_OBJECTS; j++)
                        {
                            if (!gServerEntityPool[j].isActive)
                            {
                                bulletIndex = j;
                                break;
                            }
                        }

                        if (bulletIndex != -1)
                        {
                            std::lock_guard<std::mutex> lock(gPoolMutex);
                            gServerEntityPool[bulletIndex] = bullet;
                            gRemoteEntities[bulletIndex] = bullet;
                            std::cout << "[BULLET SPAWN] Added bullet entity at index " << bulletIndex << std::endl;
                        }
                        else
                        {
                            std::cerr << "[WARN] Global bullet pool is full; cannot add new bullet." << std::endl;
                        }
                    }
                }
                else
                {
                    std::cerr << "[WARN] Incomplete or invalid bullet spawn packet received." << std::endl;
                }
            }
            else
            {
                std::cerr << "[WARN] Received bullet spawn packet with insufficient size." << std::endl;
            }
        }
        else if (packetType == SCORE_UPDATE)
        {
            ScoreUpdatePacket* pkt = reinterpret_cast<ScoreUpdatePacket*>(buffer);

            std::lock_guard<std::mutex> lock(gScoreMutex);
            gScoreBoard.clear();
            gPlayerNames.clear(); 

            for (uint32_t i = 0; i < pkt->scoreCount; ++i) {
                gScoreBoard[pkt->scores[i].playerId] = pkt->scores[i].score;
                gPlayerNames[pkt->scores[i].playerId] = pkt->scores[i].name;
            }

            uint32_t highestScore = 0;
            for (uint32_t i = 0; i < pkt->scoreCount; ++i) {
                gScoreBoard[pkt->scores[i].playerId] = pkt->scores[i].score;
                if (pkt->scores[i].score > highestScore)
                    highestScore = pkt->scores[i].score;
            }

            // Win condition check (e.g., 500 points)
            if (highestScore >= VICTORY_SCORE && gameState == GameState::Playing) {
                gameState = GameState::Win;
                std::cout << "[GAME STATE] Game over! A player has reached 500 points.\n";
            }
        }
        if (packetType == NAME_REJECTED)
        {
            std::cout << "[SERVER] Name already taken. Please restart and try a different name.\n";
            running = false; // Optional: stop game loop
            gGameRunning = false;
            continue;
        }
    }
}

void SendLocalUpdate(int clientId)
{
#pragma region Sending Player Packet
    // First, send the player's update (as before).
    PlayerUpdatePacket playerPkt{};
    playerPkt.type = PLAYER_UPDATE;
    playerPkt.playerId = (myPlayerId != 0) ? myPlayerId : static_cast<uint32_t>(clientId);
    playerPkt.pos_x = playerPosX;
    playerPkt.pos_y = playerPosY;
    playerPkt.angle = playerAngle;
    playerPkt.vel_x = playerVelX;
    playerPkt.vel_y = playerVelY;

    int sentBytes = sendto(udpSocket, reinterpret_cast<char*>(&playerPkt), sizeof(playerPkt), 0,
        reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sentBytes == SOCKET_ERROR)
    {
        std::cerr << "[ERROR] sendto PLAYER_UPDATE failed: " << WSAGetLastError() << std::endl;
    }
#pragma endregion

#pragma region Sending Bullet Packet
    //////////////////////////////////////////////////////////////////////////////////////
    // Sending Bullets
    //////////////////////////////////////////////////////////////////////////////////////
    BulletSpawnMultiPacket multiPkt{};
    multiPkt.type = BULLET_SPAWN; // Using the same type, or define a new one.
    multiPkt.count = 0;
    {
        std::lock_guard<std::mutex> lock(gPoolMutex);
        // Loop through the entire local entity pool.
        for (uint32_t i = 0; i < MAX_LOCAL_ENTITIES && multiPkt.count < 10; i++)
        {
            if (gLocalEntities[i].objectType == ObjectType::Bullet &&
                gLocalEntities[i].isActive &&
                !gLocalEntities[i].isSent)
            {
                // Prepare the bullet spawn data.
                multiPkt.bullets[multiPkt.count].playerId = myPlayerId;
                multiPkt.bullets[multiPkt.count].bulletType = 0; // Standard bullet.
                multiPkt.bullets[multiPkt.count].pos_x = gLocalEntities[i].pos_x;
                multiPkt.bullets[multiPkt.count].pos_y = gLocalEntities[i].pos_y;
                // Assuming vel_x and vel_y are already set in the local entity.
                multiPkt.bullets[multiPkt.count].vel_x = gLocalEntities[i].vel_x;
                multiPkt.bullets[multiPkt.count].vel_y = gLocalEntities[i].vel_y;
                multiPkt.bullets[multiPkt.count].damage = 10.0f;
                multiPkt.count++;

                // Mark the bullet as already sent.
                gLocalEntities[i].isSent = true;
            }
        }
    }

    // Only send the bullet packet if there is at least one bullet.
    if (multiPkt.count > 0)
    {
        sentBytes = sendto(udpSocket, reinterpret_cast<char*>(&multiPkt),
            sizeof(multiPkt.type) + sizeof(multiPkt.count) + multiPkt.count * sizeof(BulletSpawnPacket),
            0, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (sentBytes == SOCKET_ERROR)
        {
            std::cerr << "[ERROR] sendto BULLET_SPAWN multi packet failed: " << WSAGetLastError() << std::endl;
        }
    }
#pragma endregion
}

#pragma endregion

#pragma region Render
// ----------------------------------------------------------------------
// UpdateRemoteInterpolation: Interpolates remote objects based on server data.
// ----------------------------------------------------------------------
void UpdateRemoteInterpolation(float dt)
{
    const float lerpFactor = 0.1f;
    const float posThreshold = 500.0f;
    const float extrapolationFactor = 1.0f; // Predict 1 second ahead.

    std::lock_guard<std::mutex> lock(gPoolMutex);
    // Process non-bullet remote objects (assumed in first half of pool).
    for (uint32_t i = 0; i < MAX_REMOTE_OBJECTS / 2; i++)
    {
        if (!gServerEntityPool[i].isActive) continue;

        float targetPosX = gServerEntityPool[i].pos_x + gServerEntityPool[i].vel_x * extrapolationFactor;
        float targetPosY = gServerEntityPool[i].pos_y + gServerEntityPool[i].vel_y * extrapolationFactor;
        float targetRot = gServerEntityPool[i].rotation;
        float targetScale = gServerEntityPool[i].scale;

        float diffX = fabs(targetPosX - gServerEntityPool[i].pos_x);
        if (diffX > posThreshold)
            gRemoteEntities[i].pos_x = targetPosX;
        else
            gRemoteEntities[i].pos_x = Lerp(gServerEntityPool[i].pos_x, targetPosX, lerpFactor);

        float diffY = fabs(targetPosY - gServerEntityPool[i].pos_y);
        if (diffY > posThreshold)
            gRemoteEntities[i].pos_y = targetPosY;
        else
            gRemoteEntities[i].pos_y = Lerp(gServerEntityPool[i].pos_y, targetPosY, lerpFactor);

        gRemoteEntities[i].rotation = targetRot;
        gRemoteEntities[i].scale = targetScale;
        gRemoteEntities[i].objectType = gServerEntityPool[i].objectType;
        gRemoteEntities[i].playerId = gServerEntityPool[i].playerId;
        gRemoteEntities[i].vel_x = gServerEntityPool[i].vel_x;
        gRemoteEntities[i].vel_y = gServerEntityPool[i].vel_y;
        gRemoteEntities[i].entityId = gServerEntityPool[i].entityId;
        gRemoteEntities[i].isActive = gServerEntityPool[i].isActive;
    }

    // Update bullet objects by simply adding velocity to position.
    int bulletStartIndex = MAX_REMOTE_OBJECTS / 2;
    for (int i = bulletStartIndex; i < MAX_REMOTE_OBJECTS; i++)
    {
        if (!gRemoteEntities[i].isActive)
            continue;
        gRemoteEntities[i].pos_x += gRemoteEntities[i].vel_x * dt;
        gRemoteEntities[i].pos_y += gRemoteEntities[i].vel_y * dt;
        gRemoteEntities[i].rotation = gServerEntityPool[i].rotation;
        gRemoteEntities[i].scale = gServerEntityPool[i].scale;
        gRemoteEntities[i].objectType = gServerEntityPool[i].objectType;
        gRemoteEntities[i].isActive = gServerEntityPool[i].isActive;
    }
}

// ----------------------------------------------------------------------
// Render: Renders local player, local entities, and remote entities.
// ----------------------------------------------------------------------
void Render()
{
    std::lock_guard<std::mutex> lock(gPoolMutex);

    // Render Background
    {
        float windowWidth = 1600.0f;
        float windowHeight = 900.0f;
        AEMtx33 scaleMtx, rotMtx, transMtx, finalMtx;
        AEMtx33Scale(&scaleMtx, windowWidth, windowHeight);
        AEMtx33Rot(&rotMtx, 0);
        AEMtx33Trans(&transMtx, 0, 0);
        AEMtx33Concat(&finalMtx, &rotMtx, &scaleMtx);
        AEMtx33Concat(&finalMtx, &transMtx, &finalMtx);
        AEGfxTextureSet(Background1Texture, 0, 0);
        AEGfxSetTransform(finalMtx.m);
        AEGfxMeshDraw(pMesh, AE_GFX_MDM_TRIANGLES);
    }

    // Render local player.
    {
        AEMtx33 scaleMtx, rotMtx, transMtx, finalMtx;
        AEMtx33Scale(&scaleMtx, gLocalPlayer.scale, gLocalPlayer.scale);
        AEMtx33Rot(&rotMtx, gLocalPlayer.rotation + (3.1415926f / 2.0f));
        AEMtx33Trans(&transMtx, gLocalPlayer.pos_x, gLocalPlayer.pos_y);
        AEMtx33Concat(&finalMtx, &rotMtx, &scaleMtx);
        AEMtx33Concat(&finalMtx, &transMtx, &finalMtx);
        switch (gLocalPlayer.playerId)
        {
        case 1:
            AEGfxTextureSet(Player1Texture, 0, 0);
            break;
        case 2:
            AEGfxTextureSet(Player2Texture, 0, 0);
            break;
        case 3:
            AEGfxTextureSet(Player3Texture, 0, 0);
            break;
        case 4:
            AEGfxTextureSet(Player4Texture, 0, 0);
            break;
        }
        AEGfxSetTransform(finalMtx.m);
        AEGfxMeshDraw(pMesh, AE_GFX_MDM_TRIANGLES);
    }

    // Render local entities (bullets, etc.).
    for (uint32_t i = 0; i < MAX_LOCAL_ENTITIES; i++)
    {
        if (!gLocalEntities[i].isActive)
            continue;

        AEMtx33 scaleMtx, rotMtx, transMtx, finalMtx;
        AEMtx33Scale(&scaleMtx, gLocalEntities[i].scale, gLocalEntities[i].scale);
        AEMtx33Rot(&rotMtx, gLocalEntities[i].rotation + (3.1415926f / 2.0f));
        AEMtx33Trans(&transMtx, gLocalEntities[i].pos_x, gLocalEntities[i].pos_y);
        AEMtx33Concat(&finalMtx, &rotMtx, &scaleMtx);
        AEMtx33Concat(&finalMtx, &transMtx, &finalMtx);

#pragma region Texture Selection for Local Entities
#if 1
        // Choose texture based on object type.
        if (gLocalEntities[i].objectType == ObjectType::Bullet) // Bullet.
            AEGfxTextureSet(BulletTexture, 0, 0);
        else
            AEGfxTextureSet(AsteroidTexture, 0, 0); // Or any other texture for other entity types.
#endif
#pragma endregion

        AEGfxSetTransform(finalMtx.m);
        AEGfxMeshDraw(pMesh, AE_GFX_MDM_TRIANGLES);
    }

    // Render remote entities remote pool.
    for (uint32_t i = 0; i < MAX_REMOTE_OBJECTS; i++)
    {
        if (!gRemoteEntities[i].isActive)
            continue;

        if (gRemoteEntities[i].objectType == ObjectType::Asteroid)
        {
            std::lock_guard<std::mutex> lock(destroyedMutex);
            //std::cout << "[RENDER] Asteroid ID: " << gRemoteEntities[i].entityId << "\n";
            if (destroyedEntityIds.count(gRemoteEntities[i].entityId))
            {
                std::cout << "[DEBUG] Skipping asteroid ID " << gRemoteEntities[i].entityId << " during render\n";
                continue; // Skip rendering this destroyed asteroid
            }
        }

        AEMtx33 scaleMtx, rotMtx, transMtx, finalMtx;
        AEMtx33Scale(&scaleMtx, gRemoteEntities[i].scale, gRemoteEntities[i].scale);
        AEMtx33Rot(&rotMtx, gRemoteEntities[i].rotation + (3.1415926f / 2.0f));
        AEMtx33Trans(&transMtx, gRemoteEntities[i].pos_x, gRemoteEntities[i].pos_y);
        AEMtx33Concat(&finalMtx, &rotMtx, &scaleMtx);
        AEMtx33Concat(&finalMtx, &transMtx, &finalMtx);

        switch (gRemoteEntities[i].objectType)
        {
        case 1:
            AEGfxTextureSet(AsteroidTexture, 0, 0);
            break;
        case 2:
            AEGfxTextureSet(BulletTexture, 0, 0);
            break;
        default:
            switch (gRemoteEntities[i].playerId)
            {
            case 1:
                AEGfxTextureSet(Player1Texture, 0, 0);
                break;
            case 2:
                AEGfxTextureSet(Player2Texture, 0, 0);
                break;
            case 3:
                AEGfxTextureSet(Player3Texture, 0, 0);
                break;
            case 4:
                AEGfxTextureSet(Player4Texture, 0, 0);
                break;
            }
            break;
        }
        AEGfxSetTransform(finalMtx.m);
        AEGfxMeshDraw(pMesh, AE_GFX_MDM_TRIANGLES);
    }

    RenderScoreboardText();
}

void RenderWinScreen()
{
    std::lock_guard<std::mutex> lock(gPoolMutex);

    RenderScoreboardText(1); // 1 Renders to middle, 0 renders top right for in-game
}

#pragma endregion

#pragma region DO NOT TOUCH
// ----------------------------------------------------------------------
// Main Entry Point
// ----------------------------------------------------------------------
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);

    AESysInit(hInstance, nCmdShow, 1600, 900, 1, 60, true, nullptr);
    AESysSetWindowTitle("Asteroid Shooter - Client");

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "[ERROR] WSAStartup failed." << std::endl;
        return 1;
    }
    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
        std::cerr << "[ERROR] Failed to create UDP socket." << std::endl;
        WSACleanup();
        return 1;
    }
    u_long nonBlockingMode = 1;
    if (ioctlsocket(udpSocket, FIONBIO, &nonBlockingMode) != 0)
    {
        std::cerr << "[ERROR] ioctlsocket failed: " << WSAGetLastError() << std::endl;
        closesocket(udpSocket);
        WSACleanup();
        return 1;
    }

    std::string userName;
    std::cout << "Enter your name: ";
    std::getline(std::cin, userName);


    sockaddr_in clientAddr{};
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_addr.s_addr = INADDR_ANY;
    clientAddr.sin_port = 0; // ✅ No more clientId usage

    if (bind(udpSocket, reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr)) == SOCKET_ERROR)
    {
        std::cerr << "[ERROR] Bind failed." << std::endl;
        closesocket(udpSocket);
        WSACleanup();
        return 1;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    char serverIpStr[INET_ADDRSTRLEN];
    std::cout << "Enter server IP address: ";
    std::cin >> serverIpStr;
    if (inet_pton(AF_INET, serverIpStr, &serverAddr.sin_addr) <= 0)
    {
        std::cerr << "[ERROR] Invalid server IP address." << std::endl;
        closesocket(udpSocket);
        WSACleanup();
        return 1;
    }
    serverAddr.sin_port = htons(SERVER_PORT);

    // Send join request.
    JoinRequestPacket joinReq{};
    strcpy_s(joinReq.name, sizeof(joinReq.name), userName.c_str());
    sendto(udpSocket, reinterpret_cast<char*>(&joinReq), sizeof(joinReq), 0,
        reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));

    std::thread recvThread(ReceiveThread, udpSocket);

    AEInputShowCursor(1);

    // Pre-load mesh and textures.
    AEGfxMeshStart();
    AEGfxTriAdd(-0.5f, -0.5f, 0xFFFFFFFF, 0.0f, 1.0f,
        0.5f, -0.5f, 0xFFFFFFFF, 1.0f, 1.0f,
        -0.5f, 0.5f, 0xFFFFFFFF, 0.0f, 0.0f);
    AEGfxTriAdd(0.5f, -0.5f, 0xFFFFFFFF, 1.0f, 1.0f,
        0.5f, 0.5f, 0xFFFFFFFF, 1.0f, 0.0f,
        -0.5f, 0.5f, 0xFFFFFFFF, 0.0f, 0.0f);
    pMesh = AEGfxMeshEnd();

    Background1Texture = AEGfxTextureLoad("Assets/Background1.png");
    AsteroidTexture = AEGfxTextureLoad("Assets/Asteroid.png");
    PlanetTexture = AEGfxTextureLoad("Assets/PlanetTexture.png");
    Player1Texture = AEGfxTextureLoad("Assets/Ship1.png");
    Player2Texture = AEGfxTextureLoad("Assets/Ship2.png");
    Player3Texture = AEGfxTextureLoad("Assets/Ship3.png");
    Player4Texture = AEGfxTextureLoad("Assets/Ship4.png");
    BulletTexture = AEGfxTextureLoad("Assets/Bullet.png");
    pFont = AEGfxCreateFont("Assets/liberation-mono.ttf", 72); // load in font

    gGameRunning = true;
    while (gGameRunning)
    {
        AESysFrameStart();
        AEGfxSetBackgroundColor(0.f, 0.f, 0.f);
        AEGfxSetRenderMode(AE_GFX_RM_TEXTURE);
        AEGfxSetBlendMode(AE_GFX_BM_BLEND);
        AEGfxSetTransparency(1.f);
        AEGfxSetColorToMultiply(1, 1, 1, 1);
        AEGfxSetColorToAdd(0, 0, 0, 0);

        float dt = static_cast<float>(AEFrameRateControllerGetFrameTime());

        switch (gameState)
        {
        case GameState::Playing:
            UpdateLocalSimulation(dt);
            SendLocalUpdate(myPlayerId);
            UpdateRemoteInterpolation(dt);
            HandleCollisionChecks();
            Render();
            break;

        case GameState::Win:
            scoreboardAnimTimer += dt; // Advance animation
            if (scoreboardAnimTimer > scoreboardAnimDuration)
                scoreboardAnimTimer = scoreboardAnimDuration;
            RenderWinScreen();
            break;

        default:
            break;
        }

        AESysFrameEnd();
        if (AEInputCheckTriggered(AEVK_ESCAPE) || !AESysDoesWindowExist())
            gGameRunning = false;
    }

    AEGfxMeshFree(pMesh);
    AEGfxTextureUnload(Background1Texture);
    AEGfxTextureUnload(AsteroidTexture);
    AEGfxTextureUnload(PlanetTexture);
    AEGfxTextureUnload(Player1Texture);
    AEGfxTextureUnload(Player2Texture);
    AEGfxTextureUnload(Player3Texture);
    AEGfxTextureUnload(Player4Texture);
    AEGfxTextureUnload(BulletTexture);
    AEGfxDestroyFont(pFont); //Unload font
    AESysExit();


    running = false;
    if (recvThread.joinable())
        recvThread.join();

    if (myPlayerId != 0) 
    {
        uint8_t disconnectMsg[5];
        disconnectMsg[0] = DISCONNECT;
        uint32_t netId = htonl(myPlayerId);
        memcpy(&disconnectMsg[1], &netId, sizeof(uint32_t));
        sendto(udpSocket, reinterpret_cast<char*>(disconnectMsg), 5, 0,
            (sockaddr*)&serverAddr, sizeof(serverAddr));
    }

    closesocket(udpSocket);
    WSACleanup();
    return 0;
}
#pragma endregion