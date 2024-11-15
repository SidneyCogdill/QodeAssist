/* 
 * Copyright (C) 2024 Petr Mironychev
 *
 * This file is part of QodeAssist.
 *
 * QodeAssist is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * QodeAssist is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with QodeAssist. If not, see <https://www.gnu.org/licenses/>.
 */

#include "RequestHandler.hpp"
#include "Logger.hpp"

#include <QJsonDocument>
#include <QNetworkReply>

namespace QodeAssist::LLMCore {

RequestHandler::RequestHandler(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{}

void RequestHandler::sendLLMRequest(const LLMConfig &config, const QJsonObject &request)
{
    LOG_MESSAGE(QString("Sending request to llm: \nurl: %1\nRequest body:\n%2")
                   .arg(config.url.toString(),
                        QString::fromUtf8(
                            QJsonDocument(config.providerRequest).toJson(QJsonDocument::Indented))));

    QNetworkRequest networkRequest(config.url);
    prepareNetworkRequest(networkRequest, config.providerRequest);

    QNetworkReply *reply = m_manager->post(networkRequest,
                                           QJsonDocument(config.providerRequest).toJson());
    if (!reply) {
        LOG_MESSAGE("Error: Failed to create network reply");
        return;
    }

    QString requestId = request["id"].toString();
    m_activeRequests[requestId] = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, request, config]() {
        handleLLMResponse(reply, request, config);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        reply->deleteLater();
        m_activeRequests.remove(requestId);
        if (reply->error() != QNetworkReply::NoError) {
            LOG_MESSAGE(QString("Error in QodeAssist request: %1").arg(reply->errorString()));
            emit requestFinished(requestId, false, reply->errorString());
        } else {
            LOG_MESSAGE("Request finished successfully");
            emit requestFinished(requestId, true, QString());
        }
    });
}

void RequestHandler::handleLLMResponse(QNetworkReply *reply,
                                       const QJsonObject &request,
                                       const LLMConfig &config)
{
    QString &accumulatedResponse = m_accumulatedResponses[reply];

    bool isComplete = config.provider->handleResponse(reply, accumulatedResponse);

    if (config.requestType == RequestType::Fim) {
        if (!config.multiLineCompletion
            && processSingleLineCompletion(reply, request, accumulatedResponse, config)) {
            return;
        }

        if (isComplete) {
            auto cleanedCompletion = removeStopWords(accumulatedResponse,
                                                     config.promptTemplate->stopWords());
            emit completionReceived(cleanedCompletion, request, true);
        }
    } else if (config.requestType == RequestType::Chat) {
        emit completionReceived(accumulatedResponse, request, isComplete);
    }

    if (isComplete)
        m_accumulatedResponses.remove(reply);
}

bool RequestHandler::cancelRequest(const QString &id)
{
    if (m_activeRequests.contains(id)) {
        QNetworkReply *reply = m_activeRequests[id];
        reply->abort();
        m_activeRequests.remove(id);
        m_accumulatedResponses.remove(reply);
        emit requestCancelled(id);
        return true;
    }
    return false;
}

void RequestHandler::prepareNetworkRequest(QNetworkRequest &networkRequest,
                                           const QJsonObject &providerRequest)
{
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (providerRequest.contains("api_key")) {
        QString apiKey = providerRequest["api_key"].toString();
        networkRequest.setRawHeader("Authorization", QString("Bearer %1").arg(apiKey).toUtf8());
    }
}

bool RequestHandler::processSingleLineCompletion(QNetworkReply *reply,
                                                 const QJsonObject &request,
                                                 const QString &accumulatedResponse,
                                                 const LLMConfig &config)
{
    int newlinePos = accumulatedResponse.indexOf('\n');

    if (newlinePos != -1) {
        QString singleLineCompletion = accumulatedResponse.left(newlinePos).trimmed();
        singleLineCompletion = removeStopWords(singleLineCompletion,
                                               config.promptTemplate->stopWords());

        emit completionReceived(singleLineCompletion, request, true);
        m_accumulatedResponses.remove(reply);
        reply->abort();

        return true;
    }
    return false;
}

QString RequestHandler::removeStopWords(const QStringView &completion, const QStringList &stopWords)
{
    QString filteredCompletion = completion.toString();

    for (const QString &stopWord : stopWords) {
        filteredCompletion = filteredCompletion.replace(stopWord, "");
    }

    return filteredCompletion;
}

} // namespace QodeAssist::LLMCore
