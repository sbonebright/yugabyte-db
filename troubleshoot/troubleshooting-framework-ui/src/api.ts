import axios from 'axios';
import { IN_DEVELOPMENT_MODE, DEV_HOST_URL, PROD_HOST_URL } from './helpers/config';
import { Anomaly, GraphResponse } from './helpers/dtos';

// define unique names to use them as query keys
export enum QUERY_KEY {
  fetchAnamolies = 'fetchAnamolies',
  fetchGraphs = 'fetchGraphs',
  fetchQueries = 'fetchQueries'
}

class ApiService {
  // Fetches list of anomalies
  fetchAnamolies = (universeUuid: string, startTime?: Date | null, endTime?: Date | null, hostUrl?: string) => {
    const baseUrl = hostUrl ??  IN_DEVELOPMENT_MODE ? DEV_HOST_URL : PROD_HOST_URL;
    const requestURL = `${baseUrl}/anomalies`;
    const params: any = {
      universe_uuid: universeUuid
    }
    if (startTime) {
      params.startTime = startTime;
    }
    if (endTime) {
      params.endTime = endTime;
    }
    return axios.get<Anomaly[]>(requestURL, {
      params: params}).then((res) => res.data);
  };

  fetchAnamoliesById = (universeUuid: string, anomalyUuid: string, hostUrl?: string) => {
    const baseUrl = hostUrl ??  IN_DEVELOPMENT_MODE ? DEV_HOST_URL : PROD_HOST_URL;
    const requestURL = `${baseUrl}/anomalies/${anomalyUuid}`;
    const params = {
      universe_uuid: universeUuid
    }
    return axios.get<Anomaly>(requestURL, {
      params: params}).then((res) => res.data);
  };

  // Fetches graphs and supporting data for troubleshooting 
  fetchGraphs = (universeUuid: String, data: any, hostUrl?: string) => {
    const baseUrl = hostUrl ??  IN_DEVELOPMENT_MODE ? DEV_HOST_URL : PROD_HOST_URL;
    const requestUrl = `${baseUrl}/graphs`;
    return axios.post<GraphResponse[]>(requestUrl, data, {
      params: {
        universe_uuid: universeUuid
        // mocked: true
      }
    }).then((resp) => resp.data);
  };

  fetchQueries = (universeUuid: string, hostUrl?: string) => {
    const baseUrl = hostUrl ??  IN_DEVELOPMENT_MODE ? DEV_HOST_URL : PROD_HOST_URL;
    const requestURL = `${baseUrl}/queries`;
    const params = {
      universe_uuid: universeUuid
    }
    return axios.get<any>(requestURL, {
      params: params}).then((res) => res.data);
  };
}

export const TroubleshootAPI = new ApiService();
