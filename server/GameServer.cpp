#include "GameServer.hpp"

#include <QRegExp>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTime>
#include <QVariant>
#include <QDebug>

#include <QImage>
#include <QPixmap>

#include "PermaStorage.hpp"
#include "utils.hpp"

GameServer::GameServer()
    : levelMap_(48, 48)
{
    QTime midnight(0, 0, 0);
    qsrand(midnight.secsTo(QTime::currentTime()));

    timer_ = new QTimer(this);
    connect(timer_
            , &QTimer::timeout
            , this
            , &GameServer::tick);
    timer_->setInterval(1000.0f / static_cast<float>(ticksPerSecond_));

    requestHandlers_["startTesting"] = &GameServer::HandleStartTesting_;
    requestHandlers_["setUpConst"] = &GameServer::HandleSetUpConstants_;
    requestHandlers_["setUpMap"] = &GameServer::HandleSetUpMap_;
    requestHandlers_["login"] = &GameServer::HandleLogin_;
    requestHandlers_["logout"] = &GameServer::HandleLogout_;
    requestHandlers_["register"] = &GameServer::HandleRegister_;

    requestHandlers_["examine"] = &GameServer::HandleExamine_;
    requestHandlers_["getDictionary"] = &GameServer::HandleGetDictionary_;
    requestHandlers_["look"] = &GameServer::HandleLook_;
    requestHandlers_["move"] = &GameServer::HandleMove_;

    GenRandSmoothMap(levelMap_);

    QImage map(levelMap_.GetColumnCount(), levelMap_.GetRowCount(), QImage::Format_ARGB32);

    for (int i = 0; i < levelMap_.GetRowCount(); i++)
    {
        for (int j = 0; j < levelMap_.GetColumnCount(); j++)
        {
            if (levelMap_.GetCell(j, i) == '#')
            {
                map.setPixel(j, i, qRgba(0, 0, 0, 255));
            }
            else
            {
                map.setPixel(j, i, qRgba(255, 255, 255, 0));
            }
        }
    }

    map.save("level-map.png");

    players_.reserve(1000);
}

GameServer::~GameServer()
{

}

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

void GameServer::Stop()
{
    storage_.Reset();
    storage_.Disconnect();
    timer_->stop();
}

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

    if (action != "register"
        && action != "login"
        && action != "startTesting"
        && action != "setUpConst"
        && action != "setUpMap")
    {
        if (request.find("sid") == request.end()
            || sids_.find(request["sid"].toByteArray()) == sids_.end())
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

void GameServer::setWSAddress(QString address)
{
    wsAddress_ = address;
}

void GameServer::tick()
{
    float dt = (time_.elapsed() - lastTime_) * 0.001f;
    lastTime_ = time_.elapsed();

    for (auto& p : players_)
    {
        auto v = directionToVector[static_cast<unsigned>(p.GetDirection())] * playerVelocity_ * (tick_ - p.GetClientTick());
        p.SetVelocity(v);
        p.Update(dt);
        int x = p.GetPosition().x;
        int y = p.GetPosition().y;

        if (levelMap_.GetCell(x, y) == '#')
        {
            p.SetVelocity(-v);
            p.Update(dt);
        }

        p.SetDirection(EActorDirection::NONE);
    }

    QVariantMap tickMessage;
    tickMessage["tick"] = tick_;
    emit broadcastMessage(QString(QJsonDocument::fromVariant(tickMessage).toJson()));
    tick_++;
}

void GameServer::HandleSetUpConstants_(const QVariantMap& request, QVariantMap& response)
{
    playerVelocity_ = request["playerVelocity"].toFloat();
    slideThreshold_ = request["slideThreshold"].toFloat();
    ticksPerSecond_ = request["ticksPerSecond"].toInt();
    screenRowCount_ = request["screenRowCount"].toInt();
    screenColumnCount_ = request["screenColumnCount"].toInt();
}

void GameServer::HandleSetUpMap_(const QVariantMap& request, QVariantMap& response)
{
    QVariant data = request["map"];
    for (int i = 0; i < screenRowCount_; i++)
    {
        auto row = data.toStringList();
        for (int j = 0; j < screenColumnCount_; j++)
        {
            auto cell = row[j];
            if (cell != "#" || cell != ".")
            {
                WriteResult_(response, EFEMPResult::BAD_MAP);
                return;
            }
        }
    }
}

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
    }
    else
    {
        QByteArray sid;

        do
        {
            QByteArray id = QString::number(qrand()).toLatin1();
            sid = QCryptographicHash::hash(id, QCryptographicHash::Sha1);
        } while (sids_.find(sid) != sids_.end());

        sids_.insert(sid.toHex(), login);
        response["sid"] = sid.toHex();
        response["webSocket"] = wsAddress_;

        // TODO: extract to CreatePlayer
        {
            Player player;
            player.SetId(lastId);
            lastId++;
            player.SetLogin(login);

            int x;
            int y;

            do
            {
                x = rand() % (levelMap_.GetColumnCount() - 2) + 1.5;
                y = rand() % (levelMap_.GetRowCount() - 2) + 1.5;
            } while (levelMap_.GetCell(x, y) == '#');

            player.SetPosition(Vector2(x, y));
            players_.push_back(player);

            response["id"] = player.GetId();
        }
    }
}

void GameServer::HandleLogout_(const QVariantMap& request, QVariantMap& response)
{
    auto sid = request["sid"].toByteArray();
    auto iter = sids_.find(sid);
    for (int i = 0; i < players_.size(); i++)
    {
        if (players_[i].GetLogin() == iter.value())
        {
            players_.erase(players_.begin() + i);
            break;
        }
    }
    sids_.erase(iter);
}

void GameServer::HandleStartTesting_(const QVariantMap& request, QVariantMap& response)
{
    storage_.Reset();
}

void GameServer::HandleGetDictionary_(const QVariantMap& request, QVariantMap& response)
{
    QVariantMap dictionary;
    dictionary["#"] = "wall";
    dictionary["."] = "grass";
    response["dictionary"] = dictionary;
}

void GameServer::HandleMove_(const QVariantMap& request, QVariantMap& response)
{
    auto sid = request["sid"].toByteArray();
    auto login = sids_[sid];
    unsigned tick = request["tick"].toUInt();

//    qDebug() << "tick diff: " << tick_ - tick;

    auto direction = request["direction"].toString();

    for (auto& p : players_)
    {
        if (p.GetLogin() == login)
        {
            p.SetDirection(direction);
            p.SetClientTick(tick);
            return;
        }
    }

    WriteResult_(response, EFEMPResult::BAD_ID);
}

void GameServer::HandleLook_(const QVariantMap& request, QVariantMap& response)
{
    auto sid = request["sid"].toByteArray();
    auto login = sids_[sid];

    for (auto& p : players_)
    {
        if (p.GetLogin() == login)
        {
            auto pos = p.GetPosition();
            response["x"] = pos.x;
            response["y"] = pos.y;

            QVariantList rows;

            int minX = static_cast<int>(pos.x) - 4;
            int maxX = static_cast<int>(pos.x) + 4;
            int minY = static_cast<int>(pos.y) - 3;
            int maxY = static_cast<int>(pos.y) + 3;

            for (int j = minY; j <= maxY; j++)
            {
                QVariantList row;
                for (int i = minX; i <= maxX; i++)
                {
                    if (j < 0
                        || j > levelMap_.GetRowCount() - 1
                        || i < 0
                        || i > levelMap_.GetColumnCount() - 1)
                    {
                        row.push_back("#");
                    }
                    else
                    {
                        row.push_back(QString(levelMap_.GetCell(i, j)));
                    }
                }
                rows.push_back(row);
            }

            QVariantList actors;
            for (auto& p : players_)
            {
                QVariantMap actor;
                if (p.GetPosition().y >= minY
                    && p.GetPosition().y <= maxY
                    && p.GetPosition().x >= minX
                    && p.GetPosition().x <= maxX)
                {
                    actor["type"] = "player";
                    actor["x"] = p.GetPosition().x;
                    actor["y"] = p.GetPosition().y;
                    actor["id"] = p.GetId();
                }
                actors << actor;
            }

            response["map"] = rows;
            response["actors"] = actors;
            return;
        }
    }
}

void GameServer::HandleExamine_(const QVariantMap& request, QVariantMap& response)
{
    auto id = request["id"].toInt();
    for (auto& p : players_)
    {
        if (p.GetId() == id)
        {
            response["type"] = "player";
            response["login"] = p.GetLogin();
            response["x"] = p.GetPosition().x;
            response["y"] = p.GetPosition().y;
            response["id"] = p.GetId();
            return;
        }
    }

    WriteResult_(response, EFEMPResult::BAD_ID);
}

void GameServer::WriteResult_(QVariantMap& response, const EFEMPResult result)
{
    response["result"] = fempResultToString[static_cast<unsigned>(result)];
}
