#include "GameServer.hpp"

#include <QRegExp>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTime>
#include <QVariant>
#include <QDebug>
#include <QImage>
#include <QPixmap>
#include <QFile>

#include "PermaStorage.hpp"
#include "utils.hpp"

//==============================================================================
GameServer::GameServer()
    : levelMap_(64, 64)
{
    QTime midnight(0, 0, 0);
    qsrand(midnight.secsTo(QTime::currentTime()));

    timer_ = new QTimer(this);
    connect(timer_
            , &QTimer::timeout
            , this
            , &GameServer::tick);
    timer_->setInterval(1000.0f / static_cast<float>(ticksPerSecond_));

    GenRandSmoothMap(levelMap_);
    levelMap_.ExportToImage("generated-level-map.png");
    LoadLevelFromImage_("level-map.png");
    GenMonsters_();
}

//==============================================================================
GameServer::~GameServer()
{
    for (auto actor : actors_)
    {
        delete actor;
    }
}

//==============================================================================
bool GameServer::Start()
{
    if (storage_.Connect())
    {
        storage_.InitSchema();
        timer_->start();
        time_.start();
        lastTime_ = time_.elapsed();
        return true;
    }
    return false;
}

//==============================================================================
void GameServer::Stop()
{
    storage_.Reset();
    storage_.Disconnect();
    timer_->stop();
}

//==============================================================================
void GameServer::handleFEMPRequest(const QVariantMap& request, QVariantMap& response)
{
    response["action"] = request["action"];

    auto actionIt = request.find("action");
    if (actionIt == request.end())
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    QString action = actionIt.value().toString();
//    qDebug() << "FEMP action: " << action;
    auto handlerIt = requestHandlers_.find(action);

    if (handlerIt == requestHandlers_.end())
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    // TODO: extract into unordered_map
    if (sidCheckExcpetions_.count(action.toStdString()) == 0)
    {
        if (request.find("sid") == request.end()
            || sidToPlayer_.find(request["sid"].toByteArray()) == sidToPlayer_.end())
        {
            WriteResult_(response, EFEMPResult::BAD_SID);
            return;
        }
    }

    auto handler = handlerIt.value();
    (this->*handler)(request, response);

    if (response.find("result") == response.end())
    {
        WriteResult_(response, EFEMPResult::OK);
    }
}

//==============================================================================
void GameServer::HandleRegister_(const QVariantMap& request, QVariantMap& response)
{
    QString login = request["login"].toString();
    QString password = request["password"].toString();

    bool passHasInvalidChars = false;

    for (int i = 0; i < password.size(); i++)
    {
        if (!password[i].isPrint())
        {
            passHasInvalidChars = true;
            break;
        }
    }

    if (storage_.IfLoginPresent(login))
    {
        WriteResult_(response, EFEMPResult::LOGIN_EXISTS);
    }
    else if (!QRegExp("[0-9a-zA-Z]{2,36}").exactMatch(login))
    {
        WriteResult_(response, EFEMPResult::BAD_LOGIN);
    }
    else if (!QRegExp(".{6,36}").exactMatch(password)
             || passHasInvalidChars)
    {
        WriteResult_(response, EFEMPResult::BAD_PASS);
    }
    else
    {
        QByteArray salt = QString::number(qrand()).toLatin1();
        QByteArray passwordWithSalt = password.toUtf8();
        passwordWithSalt.append(salt);
        QByteArray hash = QCryptographicHash::hash(passwordWithSalt, QCryptographicHash::Sha3_256);
        storage_.AddUser(login, QString(hash.toBase64()), QString(salt.toBase64()));
    }
}

//==============================================================================
void GameServer::HandleDestroyItem_(const QVariantMap& request, QVariantMap& response)
{
#define BAD_ID(COND)\
    if (COND)\
    {\
        WriteResult_(response, EFEMPResult::BAD_ID);\
        return;\
    }\

    BAD_ID(request.find("id") == request.end());

    int id = request["id"].toInt();

    BAD_ID(idToActor_.find(id) == idToActor_.end());

    auto actor = idToActor_[id];
    Player* player = sidToPlayer_[request["sid"].toByteArray()];

    //    pickUpRadius_
    // TODO: implement

#undef BAD_ID
}

//==============================================================================
void GameServer::setWSAddress(QString address)
{
    wsAddress_ = address;
}

//==============================================================================
void GameServer::tick()
{
    float dt = (time_.elapsed() - lastTime_) * 0.001f;
    lastTime_ = time_.elapsed();

    auto collideWithGrid = [=](Actor* actor)
    {
        auto& p = *actor;

        float x = p.GetPosition().x;
        float y = p.GetPosition().y;

        bool collided = false;

        if (levelMap_.GetCell(x + 0.5f, y) == '#')
        {
            p.SetPosition(Vector2(truncf(x + 0.5f) - 0.5f, p.GetPosition().y));
            collided = true;
        }

        if (levelMap_.GetCell(x - 0.5f, y) == '#')
        {
            p.SetPosition(Vector2(round(x - 0.5f) + 0.5f, p.GetPosition().y));
            collided = true;
        }

        if (levelMap_.GetCell(x, y + 0.5f) == '#')
        {
            p.SetPosition(Vector2(p.GetPosition().x, round(y + 0.5f) - 0.5f));
            collided = true;
        }

        if (levelMap_.GetCell(x, y - 0.5f) == '#')
        {
            p.SetPosition(Vector2(p.GetPosition().x, round(y - 0.5f) + 0.5f));
            collided = true;
        }

        if (collided)
        {
            actor->OnCollideWorld();
        }
    };

    for (Actor* actor : actors_)
    {
        auto v = directionToVector[static_cast<unsigned>(actor->GetDirection())]
                 * playerVelocity_;

        actor->SetVelocity(v);
        levelMap_.RemoveActor(actor);
        actor->Update(dt);

        collideWithGrid(actor);

        auto cells = actor->GetOccupiedCells();
        for (auto p : cells)
        {
            auto neighbours = levelMap_.GetActors(p.first, p.second);
            // TODO: repetition of neighbours
            for (auto neighbour : neighbours)
            {
                if (neighbour == actor)
                {
                    continue;
                }
                Box box0(actor->GetPosition(), actor->GetSize(), actor->GetSize());
                Box box1(neighbour->GetPosition(), neighbour->GetSize(), neighbour->GetSize());
                if (box0.Intersect(box1))
                {
                    actor->OnCollideActor(neighbour);
                    neighbour->OnCollideActor(actor);
                }
            }
        }


        levelMap_.IndexActor(actor);
    }

    QVariantMap tickMessage;
    tickMessage["tick"] = tick_;
    emit broadcastMessage(QString(QJsonDocument::fromVariant(tickMessage).toJson()));
    tick_++;
}

//==============================================================================
void GameServer::HandleSetUpConstants_(const QVariantMap& request, QVariantMap& response)
{
    if (!testingStageActive_)
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    playerVelocity_ = request["playerVelocity"].toFloat();
    slideThreshold_ = request["slideThreshold"].toFloat();
    ticksPerSecond_ = request["ticksPerSecond"].toInt();
    screenRowCount_ = request["screenRowCount"].toInt();
    screenColumnCount_ = request["screenColumnCount"].toInt();
}

//==============================================================================
void GameServer::HandleSetUpMap_(const QVariantMap& request, QVariantMap& response)
{
#define BAD_MAP(COND)\
    if (COND)\
    {\
        WriteResult_(response, EFEMPResult::BAD_MAP);\
        return;\
    }\

    if (!testingStageActive_)
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    auto rows = request["map"].toList();
    int rowCount = rows.size();
    BAD_MAP(rowCount == 0);

    int columnCount = rows[0].toList().size();
    BAD_MAP(columnCount == 0);

    levelMap_.Resize(columnCount, rowCount);

    for (int i = 0; i < rowCount; i++)
    {
        auto row = rows[i].toList();
        BAD_MAP(row.size() != columnCount);

        for (int j = 0; j < columnCount; j++)
        {
            int value = row[j].toByteArray()[0];
            BAD_MAP(value != '#' && value != '.');
            levelMap_.SetCell(j, i, value);
        }
    }

#undef BAD_MAP
}

//==============================================================================
void GameServer::HandleGetConst_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(request);

    response["playerVelocity"] = playerVelocity_;
    response["slideThreshold"] = slideThreshold_;
    response["ticksPerSecond"] = ticksPerSecond_;
    response["screenRowCount"] = screenRowCount_;
    response["screenColumnCount"] = screenColumnCount_;
}

//==============================================================================
void GameServer::HandleLogin_(const QVariantMap& request, QVariantMap& response)
{
    auto login = request["login"].toString();
    auto password = request["password"].toString();

    if (!storage_.IfLoginPresent(login))
    {
        WriteResult_(response, EFEMPResult::INVALID_CREDENTIALS);
        return;
    }

    QByteArray salt = QByteArray::fromBase64(storage_.GetSalt(login).toLatin1());
    QByteArray refPassHash = QByteArray::fromBase64(storage_.GetPassHash(login).toLatin1());

    QByteArray passwordWithSalt = password.toUtf8();
    passwordWithSalt.append(salt);
    QByteArray passHash = QCryptographicHash::hash(passwordWithSalt, QCryptographicHash::Sha3_256);

    if (passHash != refPassHash)
    {
        WriteResult_(response, EFEMPResult::INVALID_CREDENTIALS);
        return;
    }

    QByteArray sid;

    do
    {
        QByteArray id = QString::number(qrand()).toLatin1();
        sid = QCryptographicHash::hash(id, QCryptographicHash::Sha1);
    } while (sidToPlayer_.find(sid) != sidToPlayer_.end());

    sid = sid.toHex();

    Player* player = CreatePlayer_(login);
    sidToPlayer_.insert(sid, player);
    response["sid"] = sid;
    response["webSocket"] = wsAddress_;
    response["id"] = player->GetId();
}

//==============================================================================
void GameServer::HandleLogout_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(response);

    auto sid = request["sid"].toByteArray();
    auto it = sidToPlayer_.find(sid);
    Player* p = it.value();
    qDebug() << "Logging out, login: " << p->GetLogin();
    sidToPlayer_.erase(it);
    KillActor_(p);
}

//==============================================================================
void GameServer::HandleStartTesting_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(request);

    if (testingStageActive_)
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    testingStageActive_ = true;
    storage_.Reset();
}

//==============================================================================
void GameServer::HandleStopTesting_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(request);

    if (!testingStageActive_)
    {
        WriteResult_(response, EFEMPResult::BAD_ACTION);
        return;
    }

    testingStageActive_ = false;
}

//==============================================================================
void GameServer::HandleGetDictionary_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(request);

    QVariantMap dictionary;
    dictionary["#"] = "wall";
    dictionary["."] = "grass";
    response["dictionary"] = dictionary;
}

//==============================================================================
void GameServer::HandleMove_(const QVariantMap& request, QVariantMap& response)
{
    Q_UNUSED(response);

    auto sid = request["sid"].toByteArray();
    unsigned tick = request["tick"].toUInt();
//    qDebug() << "tick diff: " << tick_ - tick;
    auto direction = request["direction"].toString();

    Player* p = sidToPlayer_[sid];
    p->SetDirection(direction);
    p->SetClientTick(tick);
}

//==============================================================================
void GameServer::HandleLook_(const QVariantMap& request, QVariantMap& response)
{
    auto sid = request["sid"].toByteArray();
    Player* p = sidToPlayer_[sid];

    QVariantList rows;

    auto pos = p->GetPosition();

    response["x"] = pos.x;
    response["y"] = pos.y;

    int x = GridRound(pos.x);
    int y = GridRound(pos.y);

    int xDelta = (screenColumnCount_ - 1) / 2;
    int yDelta = (screenRowCount_ - 1) / 2;

    int minX = x - xDelta;
    int maxX = x + xDelta;
    int minY = y - yDelta;
    int maxY = y + yDelta;

    QVariantList actors;
    std::unordered_set<Actor*> actorsInArea;

    for (int j = minY; j <= maxY; j++)
    {
        QVariantList row;
        for (int i = minX; i <= maxX; i++)
        {
            row.push_back(QString(levelMap_.GetCell(i, j)));

            auto& actorsInCell = levelMap_.GetActors(i, j);
            for (auto& a : actorsInCell)
            {
                actorsInArea.insert(a);
            }
        }
        rows.push_back(row);
    }


    for (auto& a : actorsInArea)
    {
        QVariantMap actor;
        actor["type"] = a->GetType();
        actor["x"] = a->GetPosition().x;
        actor["y"] = a->GetPosition().y;
        actor["id"] = a->GetId();
        actors << actor;
    }

    response["map"] = rows;
    response["actors"] = actors;
}

//==============================================================================
void GameServer::HandleExamine_(const QVariantMap& request, QVariantMap& response)
{
    auto id = request["id"].toInt();

    if (idToActor_.count(id) == 0)
    {
        WriteResult_(response, EFEMPResult::BAD_ID);
        return;
    }

    Actor* actor = idToActor_[id];
    response["type"] = actor->GetType();
    response["x"] = actor->GetPosition().x;
    response["y"] = actor->GetPosition().y;
    response["id"] = actor->GetId();

    Player* p = dynamic_cast<Player*>(actor);
    if (p != NULL)
    {
        response["login"] = p->GetLogin();
    }
}

//==============================================================================
void GameServer::WriteResult_(QVariantMap& response, const EFEMPResult result)
{
    response["result"] = fempResultToString[static_cast<unsigned>(result)];
}

//==============================================================================
void GameServer::LoadLevelFromImage_(const QString filename)
{
    QFile levelImage(filename);
    if (levelImage.exists())
    {
        QImage map;
        map.load(filename, "png");
        levelMap_.Resize(map.width(), map.height());
        for (int i = 0; i < map.height(); i++)
        {
            for (int j = 0; j < map.width(); j++)
            {
                auto color = map.pixel(j, i);
                int summ = qRed(color) + qGreen(color) + qBlue(color);
                int value = summ > (255 * 3 / 2) ? '.' : '#';
                levelMap_.SetCell(j, i, value);
            }
        }
    }
}

//==============================================================================
void GameServer::GenMonsters_()
{
    int monsterCounter = 0;
    for (int i = 0; i < levelMap_.GetRowCount(); i++)
    {
        for (int j = 0; j < levelMap_.GetColumnCount(); j++)
        {
            if (levelMap_.GetCell(j, i) != '#')
            {
                monsterCounter++;
                if (monsterCounter % 5 == 0)
                {
                    Monster* monster = CreateActor_<Monster>();
                    Monster& m = *monster;
                    SetActorPosition_(monster, Vector2(j + 0.5f, i + 0.5f));
                    m.SetDirection(static_cast<EActorDirection>(rand() % 4 + 1));
                }
            }
        }
    }
}

//==============================================================================
Player* GameServer::CreatePlayer_(const QString login)
{
    Player* player = CreateActor_<Player>();
    Player& p = *player;
    p.SetLogin(login);

    int x = 0;
    int y = 0;
    int c = 0;
    int r = 0;

    while (true)
    {
        if (levelMap_.GetCell(c, r) == '.')
        {
            x = c;
            y = r;
            break;
        }
        c++;
        if (c >= levelMap_.GetColumnCount())
        {
            c = 0;
            r++;
            if (r >= levelMap_.GetRowCount())
            {
                break;
            }
        }
    }

    SetActorPosition_(player, Vector2(x + 0.5f, y + 0.5f));

    return player;
}

//==============================================================================
void GameServer::SetActorPosition_(Actor* actor, const Vector2& position)
{
    levelMap_.RemoveActor(actor);
    actor->SetPosition(position);
    levelMap_.IndexActor(actor);
}
